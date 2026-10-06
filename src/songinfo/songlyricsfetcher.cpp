/* This file is part of Clementine.
   Copyright 2026, The Clementine Mintian Project

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

#include "songlyricsfetcher.h"
#include "lrcparser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

#include "core/logging.h"
#include "core/network.h"
#include "songinfotextview.h"

LrcLibLyricsProvider::LrcLibLyricsProvider()
    : network_(new NetworkAccessManager(this)), reply_(nullptr) {}

QString LrcLibLyricsProvider::name() const {
  return "LRCLIB";
}

void LrcLibLyricsProvider::FetchInfo(int id, const Song& metadata) {
  QUrl url("https://lrclib.net/api/get");
  QUrlQuery query;
  query.addQueryItem("artist_name", metadata.artist());
  query.addQueryItem("track_name", metadata.title());

  if (!metadata.album().isEmpty())
  query.addQueryItem("album_name", metadata.album());

  if (metadata.length_nanosec() > 0)
    query.addQueryItem(
        "duration",
        QString::number(metadata.length_nanosec() / 1'000'000'000LL));

  url.setQuery(query);

  QNetworkRequest request(url);
  request.setRawHeader(
    "User-Agent", "(https://www.clementine-player.org)");

  qLog(Debug) << "Fetching lyrics from" << url.toString();

  reply_ = network_->get(request);

  connect(reply_, &QNetworkReply::finished, this,
        [this, reply = reply_, id, metadata]() {
          RequestFinished(reply, id, metadata);
        });
}

void LrcLibLyricsProvider::Cancel(int id) {
  Q_UNUSED(id);

  if (reply_) {
    reply_->abort();
    reply_->deleteLater();
    reply_ = nullptr;
  }
}

void LrcLibLyricsProvider::StartSearch(int id, const Song& metadata) {
  QUrl url("https://lrclib.net/api/search");

  QUrlQuery query;
  query.addQueryItem("artist_name", metadata.artist());
  query.addQueryItem("track_name", metadata.title());

  url.setQuery(query);

  QNetworkRequest request(url);
  request.setRawHeader(
      "User-Agent", "(https://www.clementine-player.org)");

  qLog(Debug) << "Searching LRCLIB for" << url.toString();

  reply_ = network_->get(request);

  connect(reply_, &QNetworkReply::finished, this,
          [this, reply = reply_, id, metadata]() {
            SearchFinished(reply, id, metadata);
          });
}

void LrcLibLyricsProvider::SearchFinished(
    QNetworkReply* reply, int id, const Song& metadata) {
  reply->deleteLater();

  if (reply != reply_) return;
  reply_ = nullptr;

  qLog(Debug) << "LRCLIB search reply:"
              << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
              << reply->url();

  if (reply->error() != QNetworkReply::NoError) {
    qLog(Debug) << "LRCLIB search failed:" << reply->errorString();
    emit Finished(id);
    return;
  }

  const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());

  if (!document.isArray()) {
    qLog(Debug) << "LRCLIB search returned invalid JSON";
    emit Finished(id);
    return;
  }

  const QString wanted_artist = metadata.artist().toCaseFolded();
  const QString wanted_title = metadata.title().toCaseFolded();

  const qint64 wanted_duration =
      qRound64(double(metadata.length_nanosec()) / 1'000'000'000.0);

  QJsonObject best_result;
  qint64 best_difference = 11;

  for (const QJsonValue& value : document.array()) {
    const QJsonObject result = value.toObject();

    if (result.isEmpty()) continue;

    const QString artist = result.value("artistName").toString().toCaseFolded();
    const QString title = result.value("trackName").toString().toCaseFolded();

    if (artist != wanted_artist || title != wanted_title) continue;

    const QString plain_lyrics =
        result.value("plainLyrics").toString().trimmed();
    const QString synced_lyrics =
        result.value("syncedLyrics").toString();

    if (plain_lyrics.isEmpty() && synced_lyrics.isEmpty()) continue;

    qint64 difference = 0;

    if (wanted_duration > 0) {
      const qint64 duration =
          qRound64(result.value("duration").toDouble());

      difference = qAbs(duration - wanted_duration);

      if (difference > 10) continue;
    }

    if (best_result.isEmpty() || difference < best_difference) {
      best_result = result;
      best_difference = difference;
    }
  }

  if (best_result.isEmpty()) {
    qLog(Debug) << "LRCLIB search found no suitable lyrics";
    emit Finished(id);
    return;
  }

  QString lyrics =
      best_result.value("plainLyrics").toString().trimmed();

  if (lyrics.isEmpty()) {
    const QString synced_lyrics =
        best_result.value("syncedLyrics").toString();

    if (!synced_lyrics.isEmpty()) {
      lyrics = LrcParser::ToPlainText(synced_lyrics);
    }
  }

  if (lyrics.isEmpty()) {
    qLog(Debug) << "LRCLIB search result contained no usable lyrics";
    emit Finished(id);
    return;
  }

  CollapsibleInfoPane::Data data;
  data.id_ = "lrclib/lyrics";
  data.title_ = tr("Lyrics from %1").arg(name());
  data.type_ = CollapsibleInfoPane::Data::Type_Lyrics;
  data.relevance_ = 101;

  SongInfoTextView* editor = new SongInfoTextView;
  editor->setPlainText(lyrics);
  data.contents_ = editor;

  emit InfoReady(id, data);
  emit Finished(id);
}

void LrcLibLyricsProvider::RequestFinished(
    QNetworkReply* reply, int id, const Song& metadata) {
  reply->deleteLater();

  if (reply != reply_) return;
  reply_ = nullptr;

  qLog(Debug) << "LRCLIB reply:"
              << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
              << reply->url();

  const int status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

  if (reply->error() != QNetworkReply::NoError) {
    if (status == 404) {
      qLog(Debug) << "LRCLIB /api/get found no exact match; trying search";
      StartSearch(id, metadata);
      return;
    }

    qLog(Debug) << "LRCLIB request failed:" << reply->errorString();
    emit Finished(id);
    return;
  }

  const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());

  if (!document.isObject()) {
    qLog(Debug) << "LRCLIB returned invalid JSON";
    emit Finished(id);
    return;
  }

  const QJsonObject object = document.object();
  QString lyrics = object.value("plainLyrics").toString().trimmed();

  if (lyrics.isEmpty()) {
    const QString synced_lyrics =
        object.value("syncedLyrics").toString();

    if (!synced_lyrics.isEmpty()) {
      lyrics = LrcParser::ToPlainText(synced_lyrics);
    }
  }

  if (!lyrics.isEmpty()) {
    CollapsibleInfoPane::Data data;
    data.id_ = "lrclib/lyrics";
    data.title_ = tr("Lyrics from %1").arg(name());
    data.type_ = CollapsibleInfoPane::Data::Type_Lyrics;
    data.relevance_ = 101;

    SongInfoTextView* editor = new SongInfoTextView;
    editor->setPlainText(lyrics);
    data.contents_ = editor;

    emit InfoReady(id, data);
    emit Finished(id);
    return;
  }

  qLog(Debug) << "LRCLIB /api/get returned no lyrics; trying search";
  StartSearch(id, metadata);
}
