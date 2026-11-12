/* This file is part of Clementine.
   Copyright 2010-2012, David Sansome <me@davidsansome.com>
   Copyright 2010, 2014, John Maguire <john.maguire@gmail.com>
   Copyright 2011, Tyler Rhodes <tyler.s.rhodes@gmail.com>
   Copyright 2011, Andrea Decorte <adecorte@gmail.com>
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

#ifndef INTERNET_JAMENDO_JAMENDOSERVICE_H_
#define INTERNET_JAMENDO_JAMENDOSERVICE_H_

#include <memory>
#include "internet/core/internetservice.h"

class LibraryBackend;
class LibraryFilterWidget;
class LibraryModel;
class LibrarySearchProvider;
class QMenu;
class QSortFilterProxyModel;

class JamendoService : public InternetService {
  Q_OBJECT

 public:
  JamendoService(Application* app, InternetModel* parent);
  ~JamendoService() override;

  QStandardItem* CreateRootItem() override;
  void LazyPopulate(QStandardItem* item) override;

  void ShowContextMenu(const QPoint& global_pos) override;

  QWidget* HeaderWidget() const override;

  LibraryBackend* library_backend() const { return library_backend_.get(); }

  static const char* kServiceName;
  static const char* kHomepage;
  static const char* kSongsTable;
  static const char* kFtsTable;
  static const char* kSettingsGroup;

 private:
  void EnsureMenuCreated();

 private slots:
  void UpdateTotalSongCount(int count);

  void Homepage();

 private:
  std::shared_ptr<LibraryBackend> library_backend_;
  LibraryFilterWidget* library_filter_;
  LibraryModel* library_model_;
  QSortFilterProxyModel* library_sort_model_;
  LibrarySearchProvider* search_provider_;

  bool has_local_catalog_;
};

#endif  // INTERNET_JAMENDO_JAMENDOSERVICE_H_
