/* This file is part of Clementine.
   Copyright 2010-2013, David Sansome <me@davidsansome.com>
   Copyright 2011, Tyler Rhodes <tyler.s.rhodes@gmail.com>
   Copyright 2011, Paweł Bara <keirangtp@gmail.com>
   Copyright 2012, 2014, John Maguire <john.maguire@gmail.com>
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

#include "intergalacticfmservice.h"

#include <QDataStream>
#include <QDesktopServices>
#include <QMenu>
#include <QUrl>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPersistentModelIndex>
#include <QRegularExpression>

#include "core/application.h"
#include "core/network.h"
#include "core/player.h"
#include "core/taskmanager.h"
#include "globalsearch/globalsearch.h"
#include "globalsearch/intergalacticfmsearchprovider.h"
#include "intergalacticfmurlhandler.h"
#include "internet/core/internetmodel.h"
#include "ui/iconloader.h"

const int IntergalacticFMServiceBase::kStreamsCacheDurationSecs =
    60 * 60 * 24 * 28;  // 4 weeks

bool operator<(const IntergalacticFMServiceBase::Stream& a,
               const IntergalacticFMServiceBase::Stream& b) {
  return a.title_.compare(b.title_, Qt::CaseInsensitive) < 0;
}

IntergalacticFMServiceBase::IntergalacticFMServiceBase(
    Application* app, InternetModel* parent, const QString& name,
    const QUrl& homepage_url, const QUrl& donate_page_url, const QIcon& icon)
    : InternetService(name, app, parent, parent),
      url_scheme_(name.toLower().remove(' ')),
      url_handler_(new IntergalacticFMUrlHandler(app, this, this)),
      root_(nullptr),
      network_(new NetworkAccessManager(this)),
      streams_(name, "streams", kStreamsCacheDurationSecs),
      name_(name),
      homepage_url_(homepage_url),
      donate_page_url_(donate_page_url),
      icon_(icon) {
  ReloadSettings();
  ForceRefreshStreams();

  app_->player()->RegisterUrlHandler(url_handler_);
  app_->global_search()->AddProvider(
      new IntergalacticFMSearchProvider(this, app_, this));
}

IntergalacticFMServiceBase::~IntergalacticFMServiceBase() {}

QStandardItem* IntergalacticFMServiceBase::CreateRootItem() {
  root_ = new QStandardItem(icon_, name_);
  root_->setData(true, InternetModel::Role_CanLazyLoad);
  return root_;
}

void IntergalacticFMServiceBase::LazyPopulate(QStandardItem* item) {
  switch (item->data(InternetModel::Role_Type).toInt()) {
    case InternetModel::Type_Service:
      RefreshStreams();
      break;

    case Type_VaultDirectory:
      FetchVaultDirectory(item);
      break;

    default:
      break;
  }
}

void IntergalacticFMServiceBase::FetchVaultDirectory(QStandardItem* item) {
  const QUrl directory_url = item->data(Role_DirectoryUrl).toUrl();

  if (!directory_url.isValid()) {
    item->setData(true, InternetModel::Role_CanLazyLoad);
    return;
  }

  const QPersistentModelIndex item_index(item->index());

  QNetworkRequest request(directory_url);
  request.setTransferTimeout(30000);

  QNetworkReply* reply = network_->get(request);
  const int task_id =
      app_->task_manager()->StartTask(tr("Loading Intergalactic FM vault"));

  connect(reply, &QNetworkReply::finished, this,
          [this, reply, task_id, item_index, directory_url]() {
            app_->task_manager()->SetTaskFinished(task_id);

            const QNetworkReply::NetworkError error = reply->error();
            const QString error_string = reply->errorString();
            const QByteArray html = reply->readAll();

            reply->deleteLater();

            if (!item_index.isValid()) return;

            QStandardItem* directory = model()->itemFromIndex(item_index);
            if (!directory) return;

            if (error != QNetworkReply::NoError) {
              directory->setData(true, InternetModel::Role_CanLazyLoad);
              app_->AddError(
                  tr("Failed to load Intergalactic FM vault directory:\n%1")
                      .arg(error_string));
              return;
            }

            const QRegularExpression anchor_re(
                QStringLiteral(R"(<a\b([^>]*)>(.*?)</a\s*>)"),
                QRegularExpression::CaseInsensitiveOption |
                    QRegularExpression::DotMatchesEverythingOption);

            const QRegularExpression href_re(
                QStringLiteral(R"(\bhref\s*=\s*(['"])(.*?)\1)"),
                QRegularExpression::CaseInsensitiveOption);

            const QRegularExpression class_re(
                QStringLiteral(R"(\bclass\s*=\s*(['"])(.*?)\1)"),
                QRegularExpression::CaseInsensitiveOption);

            const QString html_text = QString::fromUtf8(html);
            auto anchors = anchor_re.globalMatch(html_text);

            while (anchors.hasNext()) {
              const QRegularExpressionMatch anchor = anchors.next();
              const QString attributes = anchor.captured(1);

              const QRegularExpressionMatch href_match =
                  href_re.match(attributes);
              const QRegularExpressionMatch class_match =
                  class_re.match(attributes);

              if (!href_match.hasMatch() || !class_match.hasMatch())
                continue;

              const QStringList classes =
                  class_match.captured(2).split(
                      QRegularExpression(QStringLiteral("\\s+")),
                      Qt::SkipEmptyParts);

              const bool is_directory = classes.contains("djsection");
              const bool is_media = classes.contains("amixlink");

              if (!is_directory && !is_media) continue;

              const QUrl link_url = QUrl::fromEncoded(
                  href_match.captured(2).toUtf8(), QUrl::TolerantMode);

              const QUrl target_url = directory_url.resolved(link_url);

              if (target_url.scheme() != "https" ||
                  target_url.host() != "hotmixxx.com") {
                continue;
              }

              QString path = target_url.path(QUrl::FullyDecoded);

              while (path.endsWith('/')) path.chop(1);

              const QString title = path.section('/', -1);
              if (title.isEmpty()) continue;

              if (is_directory) {
                if (!target_url.path().startsWith(
                        "/IntergalacticFM/")) {
                  continue;
                }

                QStandardItem* folder = new QStandardItem(
                    IconLoader::Load("folder", IconLoader::Base), title);

                folder->setData(Type_VaultDirectory,
                                InternetModel::Role_Type);
                folder->setData(target_url, Role_DirectoryUrl);
                folder->setData(true, InternetModel::Role_CanLazyLoad);

                directory->appendRow(folder);
                continue;
              }

              if (!target_url.path().startsWith(
                      "/data/videohotmix.net/IntergalacticFM/")) {
                continue;
              }

              const QString suffix = path.section('.', -1).toLower();

              if (suffix != "mp3" && suffix != "wav" && suffix != "mp4")
                continue;

              Song song;
              song.set_valid(true);
              song.set_title(title);
              song.set_artist(tr("Intergalactic FM Vault"));
              song.set_url(target_url);

              QStandardItem* media = new QStandardItem(
                  IconLoader::Load(
                      suffix == "mp4" ? "video-x-generic"
                                      : "audio-x-generic",
                      IconLoader::Base),
                  title);

              media->setData(QVariant::fromValue(song),
                             InternetModel::Role_SongMetadata);
              media->setData(InternetModel::PlayBehaviour_SingleItem,
                             InternetModel::Role_PlayBehaviour);
              media->setData(InternetModel::Type_Track,
                             InternetModel::Role_Type);

              directory->appendRow(media);
            }
          });
}

void IntergalacticFMServiceBase::ShowContextMenu(const QPoint& global_pos) {
  if (!context_menu_) {
    context_menu_.reset(new QMenu);
    context_menu_->addActions(GetPlaylistActions());
    context_menu_->addAction(IconLoader::Load("download", IconLoader::Base),
                             tr("Open %1 in browser").arg(homepage_url_.host()),
                             this, SLOT(Homepage()));

    if (!donate_page_url_.isEmpty()) {
      context_menu_->addAction(IconLoader::Load("download", IconLoader::Base),
                               tr("Donate"), this, SLOT(Donate()));
    }

    context_menu_->addAction(IconLoader::Load("view-refresh", IconLoader::Base),
                             tr("Refresh channels"), this,
                             SLOT(ForceRefreshStreams()));
  }

  context_menu_->popup(global_pos);
}

void IntergalacticFMServiceBase::ForceRefreshStreams() {
  StreamList list;

  auto add_stream = [this, &list](const QString& title,
                                  const QString& path) {
    QUrl url(QStringLiteral("http://radio.intergalactic.fm/") + path);
    url.setScheme(url_scheme_);

    list << Stream{title, name_, url};
  };

  add_stream(tr("Cybernetic Broadcasting System"), "1");
  add_stream(tr("Disco Fetish"), "2");
  add_stream(tr("The Dream Machine"), "3");

  streams_.Update(list);
  streams_.Sort();

  if (root_ && !root_->data(InternetModel::Role_CanLazyLoad).toBool()) {
    PopulateStreams();
  }

  emit StreamsChanged();
}

Song IntergalacticFMServiceBase::Stream::ToSong(const QString& prefix) const {
  QString song_title = title_.trimmed();
  if (!song_title.startsWith(prefix)) {
    song_title = prefix + " " + song_title;
  }

  Song ret;
  ret.set_valid(true);
  ret.set_title(song_title);
  ret.set_artist(dj_);
  ret.set_url(url_);
  return ret;
}

void IntergalacticFMServiceBase::Homepage() {
  QDesktopServices::openUrl(homepage_url_);
}

void IntergalacticFMServiceBase::Donate() {
  QDesktopServices::openUrl(donate_page_url_);
}

PlaylistItem::Options IntergalacticFMServiceBase::playlistitem_options() const {
  return PlaylistItem::PauseDisabled;
}

IntergalacticFMServiceBase::StreamList IntergalacticFMServiceBase::Streams() {
  if (IsStreamListStale()) {
    metaObject()->invokeMethod(this, "ForceRefreshStreams",
                               Qt::QueuedConnection);
  }
  return streams_;
}

void IntergalacticFMServiceBase::RefreshStreams() {
  if (IsStreamListStale()) {
    ForceRefreshStreams();
    return;
  }
  PopulateStreams();
}

void IntergalacticFMServiceBase::PopulateStreams() {
  if (root_->hasChildren()) root_->removeRows(0, root_->rowCount());

  // Add the three live radio stations.
  for (const Stream& stream : streams_) {
    QStandardItem* item = new QStandardItem(
        IconLoader::Load("icon_radio", IconLoader::Lastfm), QString());
    item->setText(stream.title_);
    item->setData(QVariant::fromValue(stream.ToSong(name_)),
                  InternetModel::Role_SongMetadata);
    item->setData(InternetModel::PlayBehaviour_SingleItem,
                  InternetModel::Role_PlayBehaviour);

    root_->appendRow(item);
  }

  // Add the Hotmixxx archive.
  QStandardItem* vault = new QStandardItem(
      IconLoader::Load("folder", IconLoader::Base), tr("The Vault"));

  vault->setData(Type_VaultDirectory, InternetModel::Role_Type);
  vault->setData(
      QUrl("https://hotmixxx.com/IntergalacticFM"),
      Role_DirectoryUrl);
  vault->setData(true, InternetModel::Role_CanLazyLoad);

  root_->appendRow(vault);
}

QDataStream& operator<<(QDataStream& out,
                        const IntergalacticFMServiceBase::Stream& stream) {
  out << stream.title_ << stream.dj_ << stream.url_;
  return out;
}

QDataStream& operator>>(QDataStream& in,
                        IntergalacticFMServiceBase::Stream& stream) {
  in >> stream.title_ >> stream.dj_ >> stream.url_;
  return in;
}

void IntergalacticFMServiceBase::ReloadSettings() {
  streams_.Load();
  streams_.Sort();
}

IntergalacticFMService::IntergalacticFMService(Application* app,
                                               InternetModel* parent)
    : IntergalacticFMServiceBase(
          app, parent, "Intergalactic FM",
          QUrl("https://www.intergalactic.fm"), QUrl(),
          IconLoader::Load("intergalacticfm", IconLoader::Provider)) {}
