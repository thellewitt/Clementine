/* This file is part of Clementine.
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

#ifndef STREAMDISCOVERER_H
#define STREAMDISCOVERER_H

#include <gst/pbutils/pbutils.h>

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QByteArray>

struct StreamDetails {
  QString url;
  QString format;
  int bitrate;
  int depth;
  int channels;
  int sample_rate;
};
Q_DECLARE_METATYPE(StreamDetails)

class StreamDiscoverer : public QObject {
  Q_OBJECT

 public:
  StreamDiscoverer();
  ~StreamDiscoverer();

  void Discover(const QString& url,
                const QByteArray& auth_header = QByteArray());

 signals:
  void DiscoverFinished();
  void DataReady(const StreamDetails& data);
  void Error(const QString& message);

 private:
  GstDiscoverer* discoverer_;
  QByteArray auth_header_;

  static const int kDiscoveryTimeoutS;

  // GstDiscoverer callbacks:
  static void OnDiscovered(GstDiscoverer* discoverer, GstDiscovererInfo* info,
                           GError* err, gpointer instance);
  static void OnFinished(GstDiscoverer* discoverer, gpointer instance);
  static void OnSourceSetup(GstDiscoverer* discoverer, GstElement* source,
                            gpointer self);

  // Helper to return descriptive error messages:
  static QString GSTdiscovererErrorMessage(GstDiscovererResult result);
};

#endif  // STREAMDISCOVERER_H
