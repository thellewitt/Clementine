/* This file is part of Clementine.
   Copyright 2010-2011, David Sansome <davidsansome@gmail.com>
   Copyright 2010, 2014, John Maguire <john.maguire@gmail.com>
   Copyright 2011, Arnaud Bienner <arnaud.bienner@gmail.com>
   Copyright 2014, Krzysztof Sobiecki <sobkas@gmail.com>
   Copyright 2026, Triala Boomdeay <bradfordspencer03@gmail.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "jamendodynamicplaylist.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QtDebug>

#include <algorithm>
#include <random>

#include "core/logging.h"
#include "core/network.h"
#include "core/timeconstants.h"
#include "playlist/songplaylistitem.h"

namespace {
constexpr char kJamendoClientId[] = "d0917dd8";
constexpr int kEmptyResponseRetryDelayMs = 1000;
}

const char* JamendoDynamicPlaylist::kUrl =
    "https://api.jamendo.com/v3.0/tracks/";

JamendoDynamicPlaylist::JamendoDynamicPlaylist()
    : order_by_(OrderBy_Rating),
      order_direction_(Order_Descending),
      current_index_(0),
      exhausted_(false),
      next_query_index_(0) {}

JamendoDynamicPlaylist::JamendoDynamicPlaylist(
    const QString& name, OrderBy order_by)
    : order_by_(order_by),
      order_direction_(Order_Descending),
      current_index_(0),
      exhausted_(false),
      next_query_index_(0) {
  set_name(name);
}

void JamendoDynamicPlaylist::Load(const QByteArray& data) {
  QDataStream s(data);
  s >> *this;

  current_items_.clear();
  current_index_ = 0;
  exhausted_ = false;
  seen_track_ids_.clear();
  query_states_.clear();
  next_query_index_ = 0;
}

void JamendoDynamicPlaylist::Load(OrderBy order_by,
                                  OrderDirection order_direction) {
  order_by_ = order_by;
  order_direction_ = order_direction;

  current_items_.clear();
  current_index_ = 0;
  exhausted_ = false;
  seen_track_ids_.clear();
  query_states_.clear();
  next_query_index_ = 0;
}

QByteArray JamendoDynamicPlaylist::Save() const {
  QByteArray ret;
  QDataStream s(&ret, QIODevice::WriteOnly);
  s << *this;

  return ret;
}

PlaylistItemList JamendoDynamicPlaylist::Generate() { return GenerateMore(20); }

PlaylistItemList JamendoDynamicPlaylist::GenerateMore(int count) {
  int fetch_attempts = 0;
  PlaylistItemList items;

  while (items.size() < count && !exhausted_) {
    if (current_index_ < current_items_.size()) {
      PlaylistItemList more_items =
          current_items_.mid(current_index_, count - items.size());

      items << more_items;
      current_index_ += more_items.size();
      continue;
    }

    if (fetch_attempts >= kMaxFetchAttemptsPerGenerate) break;
    ++fetch_attempts;

    if (!Fetch()) break;
  }

  return items;
}

QString JamendoDynamicPlaylist::OrderSpec(OrderBy by,
                                          OrderDirection dir) {
  QString ret;

  switch (by) {
    case OrderBy_RandomMix:
      ret = "popularity_total";
      break;
    case OrderBy_Listened:
      ret = "listens_total";
      break;
    case OrderBy_Rating:
      ret = "popularity_total";
      break;
    case OrderBy_RatingMonth:
      ret = "popularity_month";
      break;
    case OrderBy_RatingWeek:
      ret = "popularity_week";
      break;
    case OrderBy_Electronic:
    case OrderBy_IndieAlternative:
    case OrderBy_DreamPopAtmosphere:
    case OrderBy_LoungeDowntempo:
    case OrderBy_AcousticStorytellers:
    case OrderBy_JazzAfterHours:
    case OrderBy_WorldGrooves:
    case OrderBy_DynamicDiscovery:
      return QString();
  }

  switch (dir) {
    case Order_Ascending:
      ret += "_asc";
      break;
    case Order_Descending:
      ret += "_desc";
      break;
  }

  return ret;
}

void JamendoDynamicPlaylist::InitializeQueries() {
  query_states_.clear();
  next_query_index_ = 0;
  exhausted_ = false;

  auto add_query = [this](const QuerySpec& query) {
    QueryState state;
    state.query = query;
    query_states_.append(state);
  };

  auto featured_tags = [](const QString& name, const QString& tags) {
    QuerySpec query;
    query.name = name;
    query.tags = tags;
    query.featured = true;
    query.group_by_artist = true;
    return query;
  };

  auto fuzzy_tags = [](const QString& name, const QString& tags,
                       bool featured) {
    QuerySpec query;
    query.name = name;
    query.fuzzytags = tags;
    query.featured = featured;
    query.group_by_artist = true;
    return query;
  };

  switch (order_by_) {
    case OrderBy_Rating:
    case OrderBy_RatingWeek:
    case OrderBy_RatingMonth:
    case OrderBy_Listened: {
      QuerySpec query;

      switch (order_by_) {
        case OrderBy_Rating:
          query.name = "Rating";
          break;
        case OrderBy_RatingWeek:
          query.name = "Rating Week";
          break;
        case OrderBy_RatingMonth:
          query.name = "Rating Month";
          break;
        case OrderBy_Listened:
          query.name = "Most Listened";
          break;
        default:
          break;
      }

      query.order = OrderSpec(order_by_, order_direction_);
      add_query(query);
      break;
    }

    case OrderBy_RandomMix: {
      QuerySpec query;
      query.name = "Popularity Month";
      query.order = "popularity_month_desc";
      add_query(query);

      query.name = "Popularity Week";
      query.order = "popularity_week_desc";
      add_query(query);

      query.name = "Popularity Total";
      query.order = "popularity_total_desc";
      add_query(query);

      query.name = "Most Listened";
      query.order = "listens_total_desc";
      add_query(query);
      break;
    }

    case OrderBy_Electronic:
      add_query(featured_tags("Electronic", "electronic"));
      break;

    case OrderBy_IndieAlternative:
      add_query(fuzzy_tags("Indie / Alternative",
                           "rock+alternative+indie", true));
      break;

    case OrderBy_DreamPopAtmosphere:
      add_query(fuzzy_tags("Dream Pop / Atmosphere",
                           "ambient+soundtrack+relaxation", false));
      break;

    case OrderBy_LoungeDowntempo:
      add_query(fuzzy_tags("Lounge / Downtempo",
                           "lounge+relaxation+downtempo+chillout", true));
      break;

    case OrderBy_AcousticStorytellers: {
      QuerySpec query = featured_tags("Acoustic Storytellers", "songwriter");
      query.acousticelectric = "acoustic";
      add_query(query);
      break;
    }

    case OrderBy_JazzAfterHours:
      add_query(featured_tags("Jazz After Hours", "jazz"));
      break;

    case OrderBy_WorldGrooves:
      add_query(featured_tags("World Grooves", "world"));
      break;

    case OrderBy_DynamicDiscovery:
      add_query(featured_tags("Electronic", "electronic"));
      add_query(fuzzy_tags("Indie / Alternative",
                           "rock+alternative+indie", true));
      add_query(fuzzy_tags("Dream Pop / Atmosphere",
                           "ambient+soundtrack+relaxation", false));
      add_query(fuzzy_tags("Lounge / Downtempo",
                           "lounge+relaxation+downtempo+chillout", true));

      {
        QuerySpec query = featured_tags("Acoustic Storytellers", "songwriter");
        query.acousticelectric = "acoustic";
        add_query(query);
      }

      add_query(featured_tags("Jazz After Hours", "jazz"));
      add_query(featured_tags("World Grooves", "world"));
      break;

    default:
      qLog(Warning) << "Unknown Jamendo playlist type:" << order_by_;
      exhausted_ = true;
      break;
  }
}

bool JamendoDynamicPlaylist::Fetch() {
  if (exhausted_) return false;

  if (kJamendoClientId[0] == '\0') {
    qLog(Warning) << "Jamendo API client ID is not configured";
    exhausted_ = true;
    return false;
  }

  if (query_states_.isEmpty()) {
    InitializeQueries();
  }

  if (exhausted_ || query_states_.isEmpty()) return false;

  std::mt19937 rng(QRandomGenerator::global()->generate());

  const bool discovery = order_by_ >= OrderBy_Electronic;

  QList<int> query_indices;

  if (order_by_ == OrderBy_DynamicDiscovery) {
    const int query_count = query_states_.size();
    int index = next_query_index_;

    for (int scanned = 0;
         scanned < query_count && query_indices.size() < 4;
         ++scanned) {
      if (!query_states_.at(index).exhausted) {
        query_indices.append(index);
      }

      index = (index + 1) % query_count;
    }

    next_query_index_ = index;
  } else {
    for (int i = 0; i < query_states_.size(); ++i) {
      if (!query_states_.at(i).exhausted) {
        query_indices.append(i);
      }
    }
  }

  if (query_indices.isEmpty()) {
    exhausted_ = true;
    return false;
  }

  QNetworkAccessManager network;
  QJsonArray results;
  QSet<QString> batch_track_ids;

  bool successful_response = false;
  bool received_page_results = false;
  bool empty_response_retry_pending = false;

  for (int query_index : query_indices) {
    QueryState& state = query_states_[query_index];
    const QuerySpec& query = state.query;

    QUrl url(kUrl);
    QUrlQuery url_query;

    url_query.addQueryItem("client_id",
                           QString::fromLatin1(kJamendoClientId));
    url_query.addQueryItem("format", "json");
    url_query.addQueryItem("offset", QString::number(state.offset));
    url_query.addQueryItem("limit", QString::number(kPageSize));
    url_query.addQueryItem("audioformat", "mp31");
    url_query.addQueryItem("imagesize", "300");

    if (!query.order.isEmpty())
      url_query.addQueryItem("order", query.order);

    if (!query.tags.isEmpty())
      url_query.addQueryItem("tags", query.tags);

    if (!query.fuzzytags.isEmpty())
      url_query.addQueryItem("fuzzytags", query.fuzzytags);

    if (!query.acousticelectric.isEmpty()) {
      url_query.addQueryItem("acousticelectric",
                             query.acousticelectric);
    }

    if (query.featured)
      url_query.addQueryItem("featured", "1");

    if (query.group_by_artist)
      url_query.addQueryItem("groupby", "artist_id");

    url.setQuery(url_query);

    QNetworkRequest req(url);
    req.setTransferTimeout(30000);

    QNetworkReply* reply = network.get(req);

    {
      QEventLoop event_loop;
      connect(reply, SIGNAL(finished()),
              &event_loop, SLOT(quit()));
      event_loop.exec();
    }

    const QNetworkReply::NetworkError network_error = reply->error();
    const QString error_string = reply->errorString();
    const QByteArray response = reply->readAll();

    reply->deleteLater();

    if (network_error != QNetworkReply::NoError) {
      qLog(Warning) << "Jamendo request failed:"
                    << error_string
                    << "query:" << query.name
                    << "offset:" << state.offset;
      continue;
    }

    QJsonParseError parse_error;
    const QJsonDocument document =
        QJsonDocument::fromJson(response, &parse_error);

    if (parse_error.error != QJsonParseError::NoError ||
        !document.isObject()) {
      qLog(Warning) << "Invalid JSON returned from Jamendo:"
                    << parse_error.errorString()
                    << "query:" << query.name;
      continue;
    }

    const QJsonObject root = document.object();
    const QJsonObject headers = root.value("headers").toObject();
    const int api_code = headers.value("code").toInt(-1);

    if (api_code != 0) {
      qLog(Warning) << "Jamendo API error:" << api_code
                    << headers.value("error_message").toString()
                    << "query:" << query.name;

      if (api_code == 5 || api_code == 11) {
        exhausted_ = true;
        return false;
      }

      continue;
    }

    const QJsonValue results_value = root.value("results");

    if (!results_value.isArray()) {
      qLog(Warning) << "Jamendo response has no results array:"
                    << query.name
                    << "offset:" << state.offset;
      continue;
    }

    const QJsonArray page_results = results_value.toArray();
    const int reported_count =
        headers.value("results_count").toInt(-1);

    if (page_results.isEmpty()) {
      // An empty array must agree with Jamendo's reported count.
      // Otherwise this is an inconsistent response, not an empty page.
      if (reported_count != 0) {
        qLog(Warning) << "Jamendo returned an empty array with"
                      << "unexpected results_count:" << reported_count
                      << "query:" << query.name
                      << "offset:" << state.offset;
        continue;
      }

      successful_response = true;
      ++state.empty_response_count;

      if (state.empty_response_count >= kMaxEmptyResponses) {
        state.exhausted = true;

        qLog(Debug) << "Jamendo query exhausted after"
                    << state.empty_response_count
                    << "empty responses:"
                    << query.name
                    << "offset:" << state.offset;
      } else {
        empty_response_retry_pending = true;

        qLog(Debug) << "Jamendo query returned an empty response:"
                    << query.name
                    << "offset:" << state.offset
                    << "attempt:" << state.empty_response_count
                    << "of" << kMaxEmptyResponses;
      }

      continue;
    }

    successful_response = true;

    if (reported_count == 0) {
      qLog(Warning) << "Jamendo returned tracks despite reporting"
                    << "zero results:" << query.name;
    }

    state.empty_response_count = 0;
    state.offset += page_results.size();
    received_page_results = true;

    for (const QJsonValue& value : page_results) {
      if (!value.isObject()) continue;

      const QJsonObject track = value.toObject();
      const QJsonValue id_value = track.value("id");

      QString id;

      if (id_value.isString()) {
        id = id_value.toString();
      } else if (id_value.isDouble()) {
        id = QString::number(id_value.toInt());
      }

      bool ok = false;
      const int track_id = id.toInt(&ok);

      if (ok && track_id > 0 &&
          !seen_track_ids_.contains(id) &&
          !batch_track_ids.contains(id)) {
        batch_track_ids.insert(id);
        results.append(value);
      }
    }
  }

  if (!successful_response) return false;

  if (results.isEmpty()) {
    const bool all_queries_exhausted = std::all_of(
        query_states_.cbegin(), query_states_.cend(),
        [](const QueryState& state) {
          return state.exhausted;
        });

    if (all_queries_exhausted) {
      exhausted_ = true;
      qLog(Debug) << "Jamendo has no more tracks across all queries.";
      return false;
    }

    if (received_page_results) {
      qLog(Debug) << "Jamendo batch produced no new track IDs.";
    } else {
      qLog(Debug) << "Jamendo batch returned no tracks;"
                     " remaining queries will be tried.";
    }

    // Space out empty-response retries without sleeping the Qt thread.
    // This nested event loop continues processing Qt events while waiting.
    if (empty_response_retry_pending) {
      QEventLoop retry_loop;

      QTimer::singleShot(
          kEmptyResponseRetryDelayMs,
          &retry_loop, SLOT(quit()));

      retry_loop.exec(QEventLoop::ExcludeUserInputEvents);
    }

    return true;
  }

  current_items_.clear();
  current_index_ = 0;

  for (const QJsonValue& value : results) {
    const QJsonObject track = value.toObject();

    QString track_id = track.value("id").toString();

    if (track_id.isEmpty() && track.value("id").isDouble()) {
      track_id = QString::number(track.value("id").toInt());
    }

    bool id_ok = false;
    const int numeric_track_id = track_id.toInt(&id_ok);

    const QString title = track.value("name").toString();
    const QString artist = track.value("artist_name").toString();
    const QUrl audio_url(track.value("audio").toString());

    if (!id_ok || numeric_track_id <= 0 ||
        title.isEmpty() || artist.isEmpty() ||
        !audio_url.isValid() || audio_url.scheme() != "https") {
      qLog(Warning) << "Skipping incomplete Jamendo track:" << track_id;
      continue;
    }

    Song song;
    song.set_title(title);
    song.set_artist(artist);
    song.set_album(track.value("album_name").toString());
    song.set_albumartist(artist);
    song.set_track(track.value("position").toInt());

    bool year_ok = false;
    const int year =
        track.value("releasedate").toString().left(4).toInt(&year_ok);

    if (year_ok) song.set_year(year);

    song.set_length_nanosec(
        static_cast<qint64>(track.value("duration").toInt()) *
        kNsecPerSec);

    song.set_filetype(Song::Type_Stream);
    song.set_directory_id(0);
    song.set_mtime(0);
    song.set_ctime(0);
    song.set_filesize(0);
    song.set_url(audio_url);

    QString image = track.value("image").toString();

    if (image.isEmpty())
      image = track.value("album_image").toString();

    song.set_art_automatic(image);
    song.set_valid(true);

    // Only remember tracks that passed validation.
    seen_track_ids_.insert(track_id);
    current_items_ << PlaylistItemPtr(new SongPlaylistItem(song));
  }

  if (current_items_.isEmpty()) {
    qLog(Warning) << "Jamendo page contained no playable tracks; skipping.";
    return true;
  }

  if ((discovery || order_by_ == OrderBy_RandomMix) &&
      current_items_.size() > 1) {
    std::shuffle(current_items_.begin(), current_items_.end(), rng);
  }

  return true;
}

QDataStream& operator<<(QDataStream& s, const JamendoDynamicPlaylist& p) {
  s << quint8(p.order_by_) << quint8(p.order_direction_);
  return s;
}

QDataStream& operator>>(QDataStream& s, JamendoDynamicPlaylist& p) {
  quint8 order_by, order_direction;
  s >> order_by >> order_direction;
  p.order_by_ = JamendoDynamicPlaylist::OrderBy(order_by);
  p.order_direction_ = JamendoDynamicPlaylist::OrderDirection(order_direction);
  return s;
}
