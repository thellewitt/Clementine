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

#ifndef SONGINFO_LRCLIBLYRICSPROVIDER_H_
#define SONGINFO_LRCLIBLYRICSPROVIDER_H_

#include "songinfoprovider.h"

class QNetworkAccessManager;
class QNetworkReply;

class LrcLibLyricsProvider : public SongInfoProvider {
  Q_OBJECT

 public:
  LrcLibLyricsProvider();

  void FetchInfo(int id, const Song& metadata) override;
  void Cancel(int id) override;
  QString name() const override;

 private:
  void StartSearch(int id, const Song& metadata);
  void RequestFinished(QNetworkReply* reply, int id, const Song& metadata);
  void SearchFinished(QNetworkReply* reply, int id, const Song& metadata);

  QNetworkAccessManager* network_;
  QNetworkReply* reply_;
};

#endif  // SONGINFO_LRCLIBLYRICSPROVIDER_H_
