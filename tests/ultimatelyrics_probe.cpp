#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QHash>
#include <QTextDocument>
#include <QTextEdit>
#include <QElapsedTimer>
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
  song.Init("Heart of Glass", "Blondie",
            "Parallel Lines",
            348LL * 1'000'000'000LL);

  QHash<SongInfoProvider*, ProviderResult> results;
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
            result.elapsed_ms = timer.elapsed();
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
           << "providers against Heart of Glass";

  for (SongInfoProvider* provider : providers) {
    qDebug() << "Starting:" << provider->name();
    provider->FetchInfo(1, song);
  }

  QTimer::singleShot(5000, [&]() {
    qDebug() << "5-second sweep timeout";
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
