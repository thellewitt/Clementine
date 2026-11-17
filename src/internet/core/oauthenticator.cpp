/* This file is part of Clementine.
   Copyright 2012-2014, John Maguire <john.maguire@gmail.com>
   Copyright 2012, 2014, David Sansome <me@davidsansome.com>
   Copyright 2014, Arnaud Bienner <arnaud.bienner@gmail.com>
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

#include "internet/core/oauthenticator.h"

#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSslError>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

#include "core/closure.h"
#include "core/logging.h"
#include "internet/core/localredirectserver.h"

const char* OAuthenticator::kRemoteURL =
    "https://clementine-data.appspot.com/skydrive";

OAuthenticator::OAuthenticator(const QString& client_id,
                               const QString& client_secret,
                               RedirectStyle redirect, QObject* parent)
    : QObject(parent),
      client_id_(client_id),
      client_secret_(client_secret),
      redirect_style_(redirect) {}

void OAuthenticator::StartAuthorisation(const QString& oauth_endpoint,
                                        const QString& token_endpoint,
                                        const QString& scope,
                                        const QUrlQuery& extra_params) {
  token_endpoint_ = QUrl(token_endpoint);
  LocalRedirectServer* server = new LocalRedirectServer(this);
  server->Listen();

  QUrl url = QUrl(oauth_endpoint);
  QUrlQuery url_query;
  url_query.addQueryItem("response_type", "code");
  url_query.addQueryItem("client_id", client_id_);
  for (const auto& item : extra_params.queryItems()) {
    url_query.addQueryItem(item.first, item.second);
  }
  QUrl redirect_url;
  QUrlQuery redirect_url_query;

  const QString port = QString::number(server->url().port());

  if (redirect_style_ == RedirectStyle::REMOTE) {
    redirect_url = QUrl(kRemoteURL);
    redirect_url_query.addQueryItem("port", port);
  } else if (redirect_style_ == RedirectStyle::REMOTE_WITH_STATE) {
    redirect_url = QUrl(kRemoteURL);
    url_query.addQueryItem("state", port);
  } else {
    redirect_url = server->url();
  }

  redirect_url.setQuery(redirect_url_query);

  url_query.addQueryItem("redirect_uri", redirect_url.toString());
  if (!scope.isEmpty()) {  // Empty scope is valid for Dropbox.
    url_query.addQueryItem("scope", scope);
  }

  url.setQuery(url_query);

  NewClosure(server, SIGNAL(Finished()), this, &OAuthenticator::RedirectArrived,
             server, redirect_url);

  QDesktopServices::openUrl(url);
}

void OAuthenticator::RedirectArrived(LocalRedirectServer* server, QUrl url) {
  const QUrl request_url = server->request_url();
  server->deleteLater();

  QUrlQuery request_query(request_url);

  // FullyDecoded prevents delimiter-like %-escapes from corrupting
  // values that may subsequently be split.
  picked_file_ids_ =
      request_query.queryItemValue("picked_file_ids", QUrl::FullyDecoded);

  const QString authorization_code =
      request_query.queryItemValue("code", QUrl::FullyDecoded);
  const QString error =
      request_query.queryItemValue("error", QUrl::FullyDecoded);
  const QString error_description =
      request_query.queryItemValue("error_description", QUrl::FullyDecoded);

  qLog(Debug) << "OAuth redirect:"
              << "has_code =" << !authorization_code.isEmpty()
              << "error =" << error
              << "description =" << error_description;

    if (!error.isEmpty()) {
      const QString message = error_description.isEmpty()
                                  ? error
                                  : QString("%1: %2").arg(error, error_description);

      qLog(Warning) << "OAuth authorization failed:" << message;
      emit Failed(message);
      return;
    }

    if (authorization_code.isEmpty()) {
      const QString message =
          tr("OAuth redirect did not contain an authorization code");
      qLog(Error) << message;
      emit Failed(message);
      return;
    }

    RequestAccessToken(authorization_code.toUtf8(), url);
}

QByteArray OAuthenticator::ParseHttpRequest(const QByteArray& request) const {
  QList<QByteArray> split = request.split('\r');
  const QByteArray& request_line = split[0];
  QByteArray path = request_line.split(' ')[1];
  QByteArray code = path.split('=')[1];

  return code;
}

void OAuthenticator::RequestAccessToken(const QByteArray& code,
                                        const QUrl& redirect_url) {
  typedef QPair<QString, QString> Param;
  QList<Param> parameters;
  parameters << Param("code", code) << Param("client_id", client_id_)
             << Param("grant_type", "authorization_code")
             // Even though we don't use this URI anymore, it must match the
             // original one.
             << Param("redirect_uri", redirect_url.toString());
  if (!client_secret_.isEmpty()) {
    parameters << Param("client_secret", client_secret_);
  }

  QStringList params;
  for (const Param& p : parameters) {
    params.append(QString("%1=%2").arg(
        p.first, QString(QUrl::toPercentEncoding(p.second))));
  }
  QString post_data = params.join("&");
  qLog(Debug) << "Requesting OAuth access token";

  QNetworkRequest request = QNetworkRequest(QUrl(token_endpoint_));
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    "application/x-www-form-urlencoded");

  QNetworkReply* reply = network_.post(request, post_data.toUtf8());
  connect(reply, SIGNAL(sslErrors(QList<QSslError>)),
          SLOT(SslErrors(QList<QSslError>)));
  NewClosure(reply, SIGNAL(finished()), this,
             SLOT(FetchAccessTokenFinished(QNetworkReply*)), reply);
}

void OAuthenticator::FetchAccessTokenFinished(QNetworkReply* reply) {
  reply->deleteLater();

  const int status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QByteArray response_body = reply->readAll();

  QJsonParseError parse_error;
  const QJsonDocument json_document =
      QJsonDocument::fromJson(response_body, &parse_error);

  if (reply->error() != QNetworkReply::NoError || status != 200) {
    const QJsonObject error_object = json_document.object();
    QString detail = error_object["error_description"].toString();

    if (detail.isEmpty()) {
      detail = error_object["error"].toString();
    }
    if (detail.isEmpty()) {
      detail = reply->errorString();
    }
    if (detail.isEmpty()) {
      detail = tr("HTTP error %1").arg(status);
    }

    const QString message =
        tr("Failed to get OAuth access token: %1").arg(detail);

    qLog(Error) << message;
    emit Failed(message);
    return;
  }

  if (parse_error.error != QJsonParseError::NoError) {
    const QString message = tr("Failed to parse OAuth token response");
    qLog(Error) << message << parse_error.errorString();
    emit Failed(message);
    return;
  }

  const QJsonObject result = json_document.object();

  access_token_ = result["access_token"].toString();
  refresh_token_ = result["refresh_token"].toString();

  if (access_token_.isEmpty()) {
    const QString message =
        tr("OAuth token response did not contain an access token");
    qLog(Error) << message;
    emit Failed(message);
    return;
  }

  SetExpiryTime(result["expires_in"].toInt());

  emit Finished();
}

void OAuthenticator::RefreshAuthorisation(const QString& token_endpoint,
                                          const QString& refresh_token) {
  refresh_token_ = refresh_token;

  QUrl url(token_endpoint);

  typedef QPair<QString, QString> Param;
  QList<Param> parameters;
  parameters << Param("client_id", client_id_)
             << Param("client_secret", client_secret_)
             << Param("grant_type", "refresh_token")
             << Param("refresh_token", refresh_token);
  QStringList params;
  for (const Param& p : parameters) {
    params.append(QString("%1=%2").arg(
        p.first, QString(QUrl::toPercentEncoding(p.second))));
  }
  QString post_data = params.join("&");

  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    "application/x-www-form-urlencoded");
  QNetworkReply* reply = network_.post(request, post_data.toUtf8());
  NewClosure(reply, SIGNAL(finished()), this,
             SLOT(RefreshAccessTokenFinished(QNetworkReply*)), reply);
}

void OAuthenticator::SetExpiryTime(int expires_in_seconds) {
  // Set the expiry time with two minutes' grace.
  expiry_time_ = QDateTime::currentDateTime().addSecs(expires_in_seconds - 120);
  qLog(Debug) << "Current oauth access token expires at:" << expiry_time_;
}

void OAuthenticator::RefreshAccessTokenFinished(QNetworkReply* reply) {
  reply->deleteLater();

  const int status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const QByteArray response_body = reply->readAll();

  QJsonParseError parse_error;
  const QJsonDocument document =
      QJsonDocument::fromJson(response_body, &parse_error);

  if (reply->error() != QNetworkReply::NoError || status != 200) {
    const QJsonObject error_object = document.object();
    QString detail = error_object["error_description"].toString();

    if (detail.isEmpty()) {
      detail = error_object["error"].toString();
    }
    if (detail.isEmpty()) {
      detail = reply->errorString();
    }

    const QString message =
        tr("Failed to refresh OAuth access token: %1").arg(detail);

    qLog(Error) << message;
    emit Failed(message);
    return;
  }

  if (parse_error.error != QJsonParseError::NoError) {
    const QString message = tr("Failed to parse OAuth refresh response");
    qLog(Error) << message << parse_error.errorString();
    emit Failed(message);
    return;
  }

  const QJsonObject result = document.object();
  access_token_ = result["access_token"].toString();

  if (result.contains("refresh_token")) {
    refresh_token_ = result["refresh_token"].toString();
  }

  if (access_token_.isEmpty()) {
    const QString message =
        tr("OAuth refresh response did not contain an access token");
    qLog(Error) << message;
    emit Failed(message);
    return;
  }

  SetExpiryTime(result["expires_in"].toInt());
  emit Finished();
}

void OAuthenticator::SslErrors(const QList<QSslError>& errors) {
  for (const QSslError& error : errors) {
    qLog(Debug) << error.errorString();
  }
}
