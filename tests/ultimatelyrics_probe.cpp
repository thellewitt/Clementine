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

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>

#include "songinfo/ultimatelyricsreader.h"

struct ProviderResult {
  QString name;
  bool finished = false;
  bool got_lyrics = false;
  int characters = 0;
  qint64 elapsed_ms = 0;
};

int main(int argc, char** argv) {
  QApplication app(argc, argv);

  Q_INIT_RESOURCE(data);

  UltimateLyricsReader reader;
  const QList<SongInfoProvider*> providers =
      reader.Parse(":/lyrics/ultimate_providers.xml");

  qDebug() << "Loaded providers:" << providers.size();

  Song song;
  song.Init("Yellow", "Coldplay", "Parachutes", 348LL * 1'000'000'000LL);

  QHash<SongInfoProvider*, ProviderResult> results;
  QHash<SongInfoProvider*, qint64> started_ms;

  QElapsedTimer timer;
  timer.start();

  for (SongInfoProvider* provider : providers) {
    ProviderResult result;
    result.name = provider->name();
    results.insert(provider, result);

    QObject::connect(
        provider, &SongInfoProvider::InfoReady,
        [&, provider](int id, const CollapsibleInfoPane::Data& data) {
          Q_UNUSED(id);

          ProviderResult& result = results[provider];
          QString lyrics;

          if (data.contents_) {
            auto* editor = qobject_cast<QTextEdit*>(data.contents_);
            if (editor)
              lyrics = editor->toPlainText();
          } else if (data.content_object_) {
            auto* document =
                qobject_cast<QTextDocument*>(data.content_object_);
            if (document)
              lyrics = document->toPlainText();
          }

          if (!lyrics.trimmed().isEmpty()) {
            result.got_lyrics = true;
            result.characters = lyrics.size();
          }
        });

    QObject::connect(
        provider, &SongInfoProvider::Finished,
        [&, provider](int id) {
          Q_UNUSED(id);

          ProviderResult& result = results[provider];

          if (!result.finished) {
            result.finished = true;
            result.elapsed_ms =
                timer.elapsed() - started_ms.value(provider);
          }

          bool all_finished = true;

          for (SongInfoProvider* candidate : providers) {
            if (!results[candidate].finished) {
              all_finished = false;
              break;
            }
          }

          if (all_finished)
            app.quit();
        });
  }

  qDebug() << "Testing" << providers.size()
           << "providers against chosen song";

  for (SongInfoProvider* provider : providers) {
    qDebug() << "Starting:" << provider->name();

    started_ms.insert(provider, timer.elapsed());
    provider->FetchInfo(1, song);
  }

  QTimer::singleShot(5000, [&]() {
    qDebug() << "5-second sweep timeout";

    const qint64 elapsed = timer.elapsed();

    for (SongInfoProvider* provider : providers) {
      ProviderResult& result = results[provider];

      if (!result.finished) {
        result.elapsed_ms =
            elapsed - started_ms.value(provider);
      }
    }

    app.quit();
  });

  app.exec();

  qDebug() << "";
  qDebug() << "Provider results:";
  qDebug() << "-----------------------------------------------";

  for (SongInfoProvider* provider : providers) {
    const ProviderResult& result = results[provider];

    qDebug().noquote()
        << QString("%1  %2  %3 ms  %4 chars")
               .arg(result.name, -35)
               .arg(result.finished ? "FINISHED" : "TIMEOUT  ")
               .arg(result.elapsed_ms, 5)
               .arg(result.characters, 5);
  }

  qDeleteAll(providers);

  return 0;
}
