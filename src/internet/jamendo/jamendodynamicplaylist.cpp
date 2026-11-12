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
#include <QUrlQuery>
#include <QtDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSet>
#include <algorithm>
#include <random>

#include "core/logging.h"
#include "core/network.h"
#include "core/timeconstants.h"
#include "playlist/songplaylistitem.h"

namespace {
constexpr char kJamendoClientId[] = "d0917dd8";
}

const char* JamendoDynamicPlaylist::kUrl =
    "https://api.jamendo.com/v3.0/tracks/";

JamendoDynamicPlaylist::JamendoDynamicPlaylist()
    : order_by_(OrderBy_Rating),
      order_direction_(Order_Descending),
      current_page_(0),
      current_index_(0),
      exhausted_(false) {}

JamendoDynamicPlaylist::JamendoDynamicPlaylist(
    const QString& name, OrderBy order_by)
    : order_by_(order_by),
      order_direction_(Order_Descending),
      current_page_(order_by >= OrderBy_Electronic
                        ? QRandomGenerator::global()->bounded(3)
                        : 0),
      current_index_(0),
      exhausted_(false) {
  set_name(name);
}

void JamendoDynamicPlaylist::Load(const QByteArray& data) {
  QDataStream s(data);
  s >> *this;

  current_page_ = 0;
  current_items_.clear();
  current_index_ = 0;
  exhausted_ = false;
  seen_track_ids_.clear();
}

void JamendoDynamicPlaylist::Load(OrderBy order_by,
                                  OrderDirection order_direction) {
  order_by_ = order_by;
  order_direction_ = order_direction;

  current_page_ = 0;
  current_items_.clear();
  current_index_ = 0;
  exhausted_ = false;
  seen_track_ids_.clear();
}

QByteArray JamendoDynamicPlaylist::Save() const {
  QByteArray ret;
  QDataStream s(&ret, QIODevice::WriteOnly);
  s << *this;

  return ret;
}

PlaylistItemList JamendoDynamicPlaylist::Generate() { return GenerateMore(20); }

PlaylistItemList JamendoDynamicPlaylist::GenerateMore(int count) {
  int tries = 0;
  PlaylistItemList items;

  while (items.size() < count && tries++ < kApiRetryLimit &&
         !exhausted_) {
    if (current_index_ < current_items_.size()) {
      PlaylistItemList more_items =
          current_items_.mid(current_index_, count - items.size());

      items << more_items;
      current_index_ += more_items.size();
    } else {
      if (!Fetch()) break;
    }
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

bool JamendoDynamicPlaylist::Fetch() {
  if (exhausted_) return false;

  if (kJamendoClientId[0] == '\0') {
    qLog(Warning) << "Jamendo API client ID is not configured";
    exhausted_ = true;
    return false;
  }

  struct QuerySpec {
    QString order;
    QString tags;
    QString fuzzytags;
    QString acousticelectric;
    bool featured = false;
    bool group_by_artist = false;
  };

  auto featured_tags = [](const QString& tags) {
    QuerySpec query;
    query.tags = tags;
    query.featured = true;
    query.group_by_artist = true;
    return query;
  };

  auto fuzzy_tags = [](const QString& tags, bool featured = false) {
    QuerySpec query;
    query.fuzzytags = tags;
    query.featured = featured;
    query.group_by_artist = true;
    return query;
  };

  QList<QuerySpec> queries;
  QStringList legacy_orders;

  switch (order_by_) {
    case OrderBy_Rating:
    case OrderBy_RatingWeek:
    case OrderBy_RatingMonth:
    case OrderBy_Listened: {
      QuerySpec query;
      query.order = OrderSpec(order_by_, order_direction_);
      queries << query;
      break;
    }

    case OrderBy_RandomMix:
      // Retain compatibility with previously saved playlists.
      legacy_orders << "popularity_month_desc"
                    << "popularity_week_desc"
                    << "popularity_total_desc"
                    << "listens_total_desc";
      for (const QString& order : legacy_orders) {
        QuerySpec query;
        query.order = order;
        queries << query;
      }
      break;

    case OrderBy_Electronic:
      queries << featured_tags("electronic");
      break;

    case OrderBy_IndieAlternative:
      queries << fuzzy_tags("rock+alternative+indie", true);
      break;

    case OrderBy_DreamPopAtmosphere:
      queries << fuzzy_tags("ambient+soundtrack+relaxation");
      break;

    case OrderBy_LoungeDowntempo:
      queries << fuzzy_tags("lounge+relaxation+downtempo+chillout", true);
      break;

    case OrderBy_AcousticStorytellers: {
      QuerySpec query = featured_tags("songwriter");
      query.acousticelectric = "acoustic";
      queries << query;
      break;
    }

    case OrderBy_JazzAfterHours:
      queries << featured_tags("jazz");
      break;

    case OrderBy_WorldGrooves:
      queries << featured_tags("world");
      break;

    case OrderBy_DynamicDiscovery:
      queries << featured_tags("electronic")
              << fuzzy_tags("rock+alternative+indie", true)
              << fuzzy_tags("ambient+soundtrack+relaxation")
              << fuzzy_tags("lounge+relaxation+downtempo+chillout", true);

      {
        QuerySpec query = featured_tags("songwriter");
        query.acousticelectric = "acoustic";
        queries << query;
      }

      queries << featured_tags("jazz")
              << featured_tags("world");
      break;

    default:
      qLog(Warning) << "Unknown Jamendo playlist type:" << order_by_;
      exhausted_ = true;
      return false;
  }

  std::mt19937 rng(QRandomGenerator::global()->generate());

  const bool discovery = order_by_ >= OrderBy_Electronic;

  // The discovery mix deliberately samples only four categories per fetch.
  if (order_by_ == OrderBy_DynamicDiscovery) {
    std::shuffle(queries.begin(), queries.end(), rng);
    while (queries.size() > 4) queries.removeLast();
  }

  QNetworkAccessManager network;
  QJsonArray results;

  bool successful_response = false;
  bool received_track_results = false;

  for (const QuerySpec& query : queries) {
    QUrl url(kUrl);
    QUrlQuery url_query;

    url_query.addQueryItem("client_id",
                           QString::fromLatin1(kJamendoClientId));
    url_query.addQueryItem("format", "json");
    url_query.addQueryItem(
        "offset", QString::number(current_page_ * kPageSize));
    url_query.addQueryItem("limit", QString::number(kPageSize));
    url_query.addQueryItem("audioformat", "mp31");
    url_query.addQueryItem("imagesize", "300");

    if (!query.order.isEmpty())
      url_query.addQueryItem("order", query.order);

    if (!query.tags.isEmpty())
      url_query.addQueryItem("tags", query.tags);

    if (!query.fuzzytags.isEmpty())
      url_query.addQueryItem("fuzzytags", query.fuzzytags);

    if (!query.acousticelectric.isEmpty())
      url_query.addQueryItem("acousticelectric",
                             query.acousticelectric);

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
                    << error_string << ", url:" << url.toString();
      continue;
    }

    QJsonParseError parse_error;
    const QJsonDocument document =
        QJsonDocument::fromJson(response, &parse_error);

    if (parse_error.error != QJsonParseError::NoError ||
        !document.isObject()) {
      qLog(Warning) << "Invalid JSON returned from Jamendo:"
                    << parse_error.errorString();
      continue;
    }

    const QJsonObject root = document.object();
    const QJsonObject headers = root.value("headers").toObject();
    const int api_code = headers.value("code").toInt(-1);

    if (api_code != 0) {
      qLog(Warning) << "Jamendo API error:" << api_code
                    << headers.value("error_message").toString();

      if (api_code == 5 || api_code == 11) {
        exhausted_ = true;
        return false;
      }

      continue;
    }

    successful_response = true;

    const QJsonArray page_results = root.value("results").toArray();

    if (!page_results.isEmpty()) {
      received_track_results = true;
    }

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

      if (ok && track_id > 0 && !seen_track_ids_.contains(id)) {
        seen_track_ids_.insert(id);
        results.append(value);
      }
    }
  }

  if (!successful_response) return false;

  if (results.isEmpty()) {
    if (received_track_results) {
        ++current_page_;
        qLog(Debug) << "Jamendo page contained only previously seen tracks.";
        return true;
    }

    qLog(Debug) << "Jamendo returned no more tracks.";
    exhausted_ = true;
    return false;
  }

  ++current_page_;

  current_items_.clear();
  current_index_ = 0;

  for (const QJsonValue& value : results) {
    const QJsonObject track = value.toObject();

    QString track_id = track.value("id").toString();

    if (track_id.isEmpty() && track.value("id").isDouble()) {
      track_id = QString::number(track.value("id").toInt());
    }

    const QString title = track.value("name").toString();
    const QString artist = track.value("artist_name").toString();
    const QUrl audio_url(track.value("audio").toString());

    if (track_id.isEmpty() || title.isEmpty() || artist.isEmpty() ||
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
