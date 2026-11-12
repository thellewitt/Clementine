/* This file is part of Clementine.
   Copyright 2010-2013, David Sansome <me@davidsansome.com>
   Copyright 2010, 2014, John Maguire <john.maguire@gmail.com>
   Copyright 2011, Tyler Rhodes <tyler.s.rhodes@gmail.com>
   Copyright 2011, Paweł Bara <keirangtp@gmail.com>
   Copyright 2011, Andrea Decorte <adecorte@gmail.com>
   Copyright 2014, Chocobozzz <florian.bigard@gmail.com>
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

#include "jamendoservice.h"

#include <QDesktopServices>
#include <QMenu>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QUrl>

#include "core/application.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/mergedproxymodel.h"
#include "globalsearch/globalsearch.h"
#include "globalsearch/librarysearchprovider.h"
#include "internet/core/internetmodel.h"
#include "jamendodynamicplaylist.h"
#include "library/librarybackend.h"
#include "library/libraryfilterwidget.h"
#include "library/librarymodel.h"
#include "smartplaylists/generator.h"
#include "ui/iconloader.h"

const char* JamendoService::kServiceName = "Jamendo";
const char* JamendoService::kHomepage = "https://www.jamendo.com/";
const char* JamendoService::kSongsTable = "jamendo.songs";
const char* JamendoService::kFtsTable = "jamendo.songs_fts";
const char* JamendoService::kSettingsGroup = "Jamendo";

JamendoService::JamendoService(Application* app, InternetModel* parent)
    : InternetService(kServiceName, app, parent, parent),
      library_backend_(nullptr),
      library_filter_(nullptr),
      library_model_(nullptr),
      library_sort_model_(new QSortFilterProxyModel(this)),
      search_provider_(nullptr),
      has_local_catalog_(false) {
  library_backend_.reset(new LibraryBackend,
                         [](QObject* obj) { obj->deleteLater(); });
  library_backend_->moveToThread(app_->database()->thread());
  library_backend_->Init(app_->database(), kSongsTable, kFtsTable);
  connect(library_backend_.get(), SIGNAL(TotalSongCountUpdated(int)),
          SLOT(UpdateTotalSongCount(int)));

  using smart_playlists::GeneratorPtr;

  library_model_ = new LibraryModel(library_backend_, app_, this);
  library_model_->set_show_various_artists(false);
  library_model_->set_show_smart_playlists(true);

  // Migrate the old Jamendo built-in smart playlists.
  {
    struct SavedPlaylist {
      QString name;
      QString type;
      QByteArray data;
    };

    QSettings settings;
    settings.beginGroup(LibraryModel::kSmartPlaylistsSettingsGroup);

    constexpr char kMigrationKey[] =
        "jamendo_discovery_defaults_migration";

    if (settings.value(kMigrationKey, 0).toInt() < 1) {
      const QStringList obsolete_jamendo_names = {
          tr("Jamendo Top Tracks of the Month"),
          tr("Jamendo Top Tracks of the Week"),
          tr("Jamendo Top Tracks"),
          tr("Jamendo Most Listened Tracks"),
          tr("Dynamic Random Mix (Jamendo)"),
          tr("Top Tracks of Month"),
          tr("Top Tracks of Week"),
          tr("Top Tracks"),
          tr("Most Listened"),
          QStringLiteral("Jamendo Top Tracks of the Month"),
          QStringLiteral("Jamendo Top Tracks of the Week"),
          QStringLiteral("Jamendo Top Tracks"),
          QStringLiteral("Jamendo Most Listened Tracks"),
          QStringLiteral("Dynamic Random Mix (Jamendo)"),
          QStringLiteral("Top Tracks of Month"),
          QStringLiteral("Top Tracks of Week"),
          QStringLiteral("Top Tracks"),
          QStringLiteral("Most Listened")};

      const QStringList obsolete_query_names = {
          tr("Dynamic random mix"),
          QStringLiteral("Dynamic random mix")};

      QList<SavedPlaylist> retained;

      const int count = settings.beginReadArray(kSongsTable);

      for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);

        SavedPlaylist entry{
            settings.value("name").toString(),
            settings.value("type").toString(),
            settings.value("data").toByteArray()};

        const bool obsolete_jamendo =
            entry.type == "Jamendo" &&
            obsolete_jamendo_names.contains(entry.name,
                                             Qt::CaseInsensitive);

        const bool obsolete_query =
            entry.type == "Query" &&
            obsolete_query_names.contains(entry.name,
                                           Qt::CaseInsensitive);

        if (!obsolete_jamendo && !obsolete_query) {
          retained << entry;
        }
      }

      settings.endArray();

      // Rewrite the array, preserving all unrelated saved playlists.
      settings.beginWriteArray(kSongsTable, retained.size());

      for (int i = 0; i < retained.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue("name", retained[i].name);
        settings.setValue("type", retained[i].type);
        settings.setValue("data", retained[i].data);
      }

      settings.endArray();

      // Let the new default batch be installed on the next model reset.
      settings.setValue(QString::fromLatin1(kSongsTable) + "_version", 0);
      settings.setValue(kMigrationKey, 1);

      qLog(Info) << "Migrated Jamendo smart playlists:"
                 << count - retained.size() << "obsolete defaults removed;"
                 << retained.size() << "saved playlists preserved.";
    }

    settings.endGroup();
  }

  library_model_->set_default_smart_playlists(
      LibraryModel::DefaultGenerators()
      << (LibraryModel::GeneratorList()
          << GeneratorPtr(new JamendoDynamicPlaylist(
                 tr("Electronic Horizons"),
                 JamendoDynamicPlaylist::OrderBy_Electronic))
          << GeneratorPtr(new JamendoDynamicPlaylist(
                 tr("Indie & Alternative"),
                 JamendoDynamicPlaylist::OrderBy_IndieAlternative))
          << GeneratorPtr(new JamendoDynamicPlaylist(
                 tr("Dream Pop & Atmosphere"),
                 JamendoDynamicPlaylist::OrderBy_DreamPopAtmosphere))
          << GeneratorPtr(new JamendoDynamicPlaylist(
                 tr("Lounge & Downtempo"),
                 JamendoDynamicPlaylist::OrderBy_LoungeDowntempo))
          << GeneratorPtr(new JamendoDynamicPlaylist(
                 tr("Acoustic Storytellers"),
                 JamendoDynamicPlaylist::OrderBy_AcousticStorytellers))
          << GeneratorPtr(new JamendoDynamicPlaylist(
                 tr("Jazz After Hours"),
                 JamendoDynamicPlaylist::OrderBy_JazzAfterHours))
          << GeneratorPtr(new JamendoDynamicPlaylist(
                 tr("World Grooves"),
                 JamendoDynamicPlaylist::OrderBy_WorldGrooves))
          << GeneratorPtr(new JamendoDynamicPlaylist(
                 tr("Dynamic Discovery Mix"),
                 JamendoDynamicPlaylist::OrderBy_DynamicDiscovery))));

  library_model_->Reset();

  library_sort_model_->setSourceModel(library_model_);
  library_sort_model_->setSortRole(LibraryModel::Role_SortText);
  library_sort_model_->setDynamicSortFilter(true);
  library_sort_model_->setSortLocaleAware(true);
  library_sort_model_->sort(0);

  search_provider_ = new LibrarySearchProvider(
      library_backend_.get(), tr("Jamendo"), "jamendo",
      IconLoader::Load("jamendo", IconLoader::Provider), false, app_, this);
  app_->global_search()->AddProvider(search_provider_);
}

JamendoService::~JamendoService() {}

QStandardItem* JamendoService::CreateRootItem() {
  QStandardItem* item = new QStandardItem(
      IconLoader::Load("jamendo", IconLoader::Provider), kServiceName);
  item->setData(true, InternetModel::Role_CanLazyLoad);
  return item;
}

void JamendoService::LazyPopulate(QStandardItem* item) {
  switch (item->data(InternetModel::Role_Type).toInt()) {
    case InternetModel::Type_Service: {
      model()->merged_model()->AddSubModel(item->index(),
                                           library_sort_model_);
      break;
    }
    default:
      break;
  }
}

void JamendoService::UpdateTotalSongCount(int count) {
  if (count > 0) {
    has_local_catalog_ = true;
  }
}

void JamendoService::EnsureMenuCreated() {
  if (!context_menu_) {
    context_menu_.reset(new QMenu);
    context_menu_->addActions(GetPlaylistActions());
    context_menu_->addSeparator();
    context_menu_->addAction(
        IconLoader::Load("download", IconLoader::Base),
        tr("Open %1 in browser").arg("jamendo.com"), this,
        SLOT(Homepage()));
  }

  if (has_local_catalog_ && !library_filter_) {
    library_filter_ = new LibraryFilterWidget(0);
    library_filter_->SetSettingsGroup(kSettingsGroup);
    library_filter_->SetLibraryModel(library_model_);
    library_filter_->SetFilterHint(tr("Search Jamendo"));
    library_filter_->SetAgeFilterEnabled(false);

    context_menu_->addSeparator();
    context_menu_->addMenu(library_filter_->menu());
  }
}

QWidget* JamendoService::HeaderWidget() const {
  const_cast<JamendoService*>(this)->EnsureMenuCreated();
  return library_filter_;
}

void JamendoService::ShowContextMenu(const QPoint& global_pos) {
  EnsureMenuCreated();

  const bool enabled =
      model()->current_index().model() == library_sort_model_;

  GetAppendToPlaylistAction()->setVisible(true);
  GetAppendToPlaylistAction()->setEnabled(enabled);
  GetReplacePlaylistAction()->setVisible(true);
  GetReplacePlaylistAction()->setEnabled(enabled);
  GetOpenInNewPlaylistAction()->setVisible(true);
  GetOpenInNewPlaylistAction()->setEnabled(enabled);

  context_menu_->popup(global_pos);
}

void JamendoService::Homepage() { QDesktopServices::openUrl(QUrl(kHomepage)); }
