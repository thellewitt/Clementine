/* This file is part of Clementine.
   Copyright 2010, David Sansome <me@davidsansome.com>

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

#include "ultimatelyricsprovider.h"

#include <QCoreApplication>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTextCodec>
#include <QThread>
#include <QTextDocument>

#include "core/logging.h"
#include "core/network.h"
#include "songinfotextview.h"
#include "ultimatelyricslyric.h"

namespace {

QString RemoveGeniusExcludedElements(QString html) {
  static const QRegularExpression excluded_element_re(
      QString::fromLatin1(
          R"(<\s*([a-z][a-z0-9:-]*)\b(?=[^>]*\bdata-exclude-from-selection\s*=\s*["']true["'])[^>]*>)"),
      QRegularExpression::CaseInsensitiveOption);

  while (true) {
    const QRegularExpressionMatch excluded =
        excluded_element_re.match(html);

    if (!excluded.hasMatch()) break;

    const QString tag_name = excluded.captured(1);
    const QString tag_pattern =
        QString::fromLatin1(
            R"(<\s*(/?)\s*%1\b(?:"[^"]*"|'[^']*'|[^'">])*>)")
            .arg(QRegularExpression::escape(tag_name));

    const QRegularExpression tag_re(
        tag_pattern, QRegularExpression::CaseInsensitiveOption);

    const int element_start = excluded.capturedStart();
    int element_end = -1;

    if (excluded.captured(0).trimmed().endsWith(QStringLiteral("/>"))) {
      element_end = excluded.capturedEnd();
    } else {
      int depth = 1;
      int position = excluded.capturedEnd();

      while (true) {
        const QRegularExpressionMatch tag = tag_re.match(html, position);
        if (!tag.hasMatch()) break;

        position = tag.capturedEnd();

        if (!tag.captured(1).isEmpty()) {
          --depth;
          if (depth == 0) {
            element_end = tag.capturedEnd();
            break;
          }
        } else if (!tag.captured(0).trimmed().endsWith(
                       QStringLiteral("/>"))) {
          ++depth;
        }
      }

      // If malformed markup leaves this element unclosed, don't allow
      // its remaining contents to contaminate the lyrics.
      if (element_end < 0) element_end = html.size();
    }

    html.remove(element_start, element_end - element_start);
  }

  return html;
}

QString ExtractGeniusLyrics(const QString& page) {
  const QRegularExpression container_re(
      QString::fromLatin1(
          R"(<div\b(?=[^>]*\bdata-lyrics-container\s*=\s*["']true["'])[^>]*>)"),
      QRegularExpression::CaseInsensitiveOption);

  const QRegularExpression div_re(
      QString::fromLatin1(
          R"(<\s*(/?)\s*div\b(?:"[^"]*"|'[^']*'|[^'">])*>)"),
      QRegularExpression::CaseInsensitiveOption);

  QStringList lyric_parts;
  int search_position = 0;

  while (true) {
    const QRegularExpressionMatch container =
        container_re.match(page, search_position);

    if (!container.hasMatch()) break;

    // Find the matching closing div, accounting for nested div elements.
    int depth = 1;
    int position = container.capturedEnd();
    int container_end = -1;

    while (true) {
      const QRegularExpressionMatch tag = div_re.match(page, position);
      if (!tag.hasMatch()) break;

      position = tag.capturedEnd();

      if (!tag.captured(1).isEmpty()) {
        --depth;
        if (depth == 0) {
          container_end = tag.capturedStart();
          search_position = tag.capturedEnd();
          break;
        }
      } else if (!tag.captured(0).trimmed().endsWith(
                     QStringLiteral("/>"))) {
        ++depth;
      }
    }

    if (container_end < 0) break;

    QString fragment = page.mid(
        container.capturedEnd(),
        container_end - container.capturedEnd());

    fragment = RemoveGeniusExcludedElements(fragment);

    // Let Qt decode HTML entities and convert <br> elements to line breaks.
    QTextDocument document;
    document.setHtml(fragment);

    QString lyrics = document.toPlainText();

    lyrics.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    lyrics.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    lyrics.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    lyrics.replace(QChar::LineSeparator, QLatin1Char('\n'));

    lyrics.replace(
        QRegularExpression(QString::fromLatin1(R"([ \t]+\n)")),
        QStringLiteral("\n"));

    // Preserve stanza breaks without accumulating excessive blank lines.
    lyrics.replace(
        QRegularExpression(QString::fromLatin1(R"(\n{3,})")),
        QStringLiteral("\n\n"));

    lyrics = lyrics.trimmed();

    if (!lyrics.isEmpty()) lyric_parts << lyrics;
  }

  if (lyric_parts.isEmpty()) return QString();

  // Genius sometimes splits lyrics across several containers.
  QString lyrics = lyric_parts.join(QStringLiteral("\n\n"));

  // Return safe HTML for the existing SongInfoTextView rendering path.
  lyrics = lyrics.toHtmlEscaped();
  lyrics.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));

  return lyrics;
}

}  // namespace

const int UltimateLyricsProvider::kRedirectLimit = 5;

UltimateLyricsProvider::UltimateLyricsProvider()
    : network_(new NetworkAccessManager(this)),
      timeouts_(new NetworkTimeouts(30000, this)),  // 30s
      relevance_(0),
      redirect_count_(0),
      url_hop_(false) {}

void UltimateLyricsProvider::FetchInfo(int id, const Song& metadata) {
  // Get the text codec
  const QTextCodec* codec =
      QTextCodec::codecForName(charset_.toLatin1().constData());
  if (!codec) {
    qLog(Warning) << "Invalid codec" << charset_;
    emit Finished(id);
    return;
  }

  // Fill in fields in the URL
  QString url_text(url_);
  ReplaceFields(metadata, &url_text);

  QUrl url(url_text);
  qLog(Debug) << "Fetching lyrics from" << url_text;

  // Fetch the URL, follow redirects
  metadata_ = metadata;
  redirect_count_ = 0;
  url_hop_ = false;
  url_hop_rule_index_ = -1;
  QNetworkReply* reply = network_->get(QNetworkRequest(url));
  connect(reply, &QNetworkReply::finished,
          [=] { this->RequestFinished(reply, url_text, id); });
  timeouts_->AddReply(reply);
}

void UltimateLyricsProvider::RequestFinished(QNetworkReply* reply,
                                             const QString& orig_url, int id) {
  reply->deleteLater();

  const int status =
    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    qLog(Debug) << name_ << "HTTP status:" << status
                << "URL:" << reply->url();

  if (reply->error() != QNetworkReply::NoError) {
    qLog(Debug) << "Reply error" << reply->errorString();
    url_hop_ = false;
    emit Finished(id);
    return;
  }

  // Handle redirects
  QVariant redirect_target =
      reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
  if (redirect_target.isValid()) {
    if (redirect_count_ >= kRedirectLimit) {
      qLog(Debug) << "Too many redirects from" << orig_url << "to"
                  << reply->url().toString();
      url_hop_ = false;
      emit Finished(id);
      return;
    }

    QUrl target = redirect_target.toUrl();
    if (target.scheme().isEmpty() || target.host().isEmpty()) {
      QString path = target.path();
      target = reply->url();
      target.setPath(path);
    }

    redirect_count_++;
    QNetworkReply* reply = network_->get(QNetworkRequest(target));
    connect(reply, &QNetworkReply::finished,
            [=] { this->RequestFinished(reply, orig_url, id); });
    timeouts_->AddReply(reply);
    return;
  }

  const QTextCodec* codec =
      QTextCodec::codecForName(charset_.toLatin1().constData());
  const QString original_content = codec->toUnicode(reply->readAll());
  QString lyrics;
  // Check for invalid indicators
  for (const QString& indicator : invalid_indicators_) {
    if (original_content.contains(indicator)) {
      qLog(Debug) << "Found invalid indicator" << indicator;
      url_hop_ = false;
      emit Finished(id);
      return;
    }
  }

  if (!url_hop_ && name_ == QStringLiteral("genius.com")) {
    lyrics = ExtractGeniusLyrics(original_content);
  } else {
    // Start after the rule that initiated the previous URL hop.
    const int first_rule_index =
        url_hop_ ? url_hop_rule_index_ + 1 : 0;

    // Some providers return the actual lyrics directly from the
    // URL hop, without needing another extraction rule.
    if (url_hop_ && first_rule_index >= extract_rules_.size()) {
      lyrics = original_content;
    } else {
      for (int rule_index = first_rule_index;
           rule_index < extract_rules_.size(); ++rule_index) {

        Rule rule_copy = extract_rules_.at(rule_index);

        // Substitute metadata in both extraction boundaries.
        for (Rule::iterator it = rule_copy.begin();
             it != rule_copy.end(); ++it) {
          ReplaceFields(metadata_, &it->first);
          ReplaceFields(metadata_, &it->second);
        }

        QString content = original_content;

        if (ApplyExtractRule(rule_copy, &content)) {
          QUrl url(content);

          if (!url.isValid() || url.scheme().isEmpty() ||
              url.host().isEmpty()) {
            qLog(Warning) << name_
                          << "extracted invalid URL:" << content;
            continue;
          }

          url_hop_ = true;
          url_hop_rule_index_ = rule_index;

          qLog(Debug) << "Next url hop:" << url;

          QNetworkReply* next_reply =
              network_->get(QNetworkRequest(url));

          connect(next_reply, &QNetworkReply::finished,
                  [=] {
                    this->RequestFinished(next_reply, orig_url, id);
                  });

          timeouts_->AddReply(next_reply);
          return;
        }

        // Apply exclude rules.
        for (const Rule& exclude_rule : exclude_rules_) {
          ApplyExcludeRule(exclude_rule, &content);
        }

        if (!content.isEmpty() && HTMLHasAlphaNumeric(content)) {
          lyrics = content;
          break;
        }
      }
    }
  }

  if (!lyrics.isEmpty() and HTMLHasAlphaNumeric(lyrics)) {
    CollapsibleInfoPane::Data data;
    data.id_ = "ultimatelyrics/" + name_;
    data.title_ = tr("Lyrics from %1").arg(name_);
    data.type_ = CollapsibleInfoPane::Data::Type_Lyrics;
    data.relevance_ = relevance();

    if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
      SongInfoTextView* editor = new SongInfoTextView;
      editor->SetHtml(lyrics);
      data.contents_ = editor;
    } else {
      UltimateLyricsLyric* editor = new UltimateLyricsLyric;
      editor->SetHtml(lyrics);
      data.content_object_ = editor;
    }

    emit InfoReady(id, data);
  } else {
    qLog(Debug) << name_ << "returned no extractable lyrics";
  }

  url_hop_ = false;
  emit Finished(id);
}

bool UltimateLyricsProvider::ApplyExtractRule(const Rule& rule,
                                              QString* content) const {
  for (const RuleItem& item : rule) {
    if (item.second.isNull()) {
      if (item.first.startsWith("http://", Qt::CaseInsensitive) ||
          item.first.startsWith("https://", Qt::CaseInsensitive)) {
        *content = ExtractUrl(*content, rule);
        return true;
      } else {
        *content = ExtractXmlTag(*content, item.first);
      }
    } else {
      *content = Extract(*content, item.first, item.second);
    }
  }
  return false;
}

QString UltimateLyricsProvider::ExtractUrl(const QString& source,
                                           const Rule& rule) {
  QString url;
  QString id;

  for (const RuleItem& item : rule) {
    if ((item.first.startsWith("http://", Qt::CaseInsensitive) ||
         item.first.startsWith("https://", Qt::CaseInsensitive)) &&
        item.second.isNull()) {
      url = item.first;
    } else {
      id = Extract(source, item.first, item.second);
    }
  }

  url.replace("{id}", id);

  return url;
}

QString UltimateLyricsProvider::ExtractXmlTag(const QString& source,
                                              const QString& tag) {
  static const QRegularExpression re("<(\\w+).*>");  // ಠ_ಠ
  const QRegularExpressionMatch match = re.match(tag);
  if (!match.hasMatch()) return QString();

  return Extract(source, tag, "</" + match.captured(1) + ">");
}

QString UltimateLyricsProvider::Extract(const QString& source,
                                        const QString& begin,
                                        const QString& end) {
  int begin_idx = source.indexOf(begin);
  if (begin_idx == -1) return QString();
  begin_idx += begin.length();

  int end_idx = source.indexOf(end, begin_idx);
  if (end_idx == -1) return QString();

  return source.mid(begin_idx, end_idx - begin_idx);
}

void UltimateLyricsProvider::ApplyExcludeRule(const Rule& rule,
                                              QString* content) const {
  for (const RuleItem& item : rule) {
    if (item.second.isNull()) {
      *content = ExcludeXmlTag(*content, item.first);
    } else {
      *content = Exclude(*content, item.first, item.second);
    }
  }
}

QString UltimateLyricsProvider::ExcludeXmlTag(const QString& source,
                                              const QString& tag) {
  static const QRegularExpression re("<(\\w+).*>");  // ಠ_ಠ
  const QRegularExpressionMatch match = re.match(tag);
  if (!match.hasMatch()) return source;

  return Exclude(source, tag, "</" + match.captured(1) + ">");
}

QString UltimateLyricsProvider::Exclude(const QString& source,
                                        const QString& begin,
                                        const QString& end) {
  int begin_idx = source.indexOf(begin);
  if (begin_idx == -1) return source;

  int end_idx = source.indexOf(end, begin_idx + begin.length());
  if (end_idx == -1) return source;

  return source.left(begin_idx) +
         source.right(source.length() - end_idx - end.length());
}

QString UltimateLyricsProvider::FirstChar(const QString& text) {
  if (text.isEmpty()) return QString();
  return text[0].toLower();
}

QString UltimateLyricsProvider::TitleCase(const QString& text) {
  if (text.length() == 0) return QString();

  QString ret = text;
  bool last_was_space = true;

  for (QString::iterator it = ret.begin(); it != ret.end(); ++it) {
    if (last_was_space) {
      *it = it->toUpper();
      last_was_space = false;
    } else if (it->isSpace()) {
      last_was_space = true;
    }
  }

  return ret;
}

void UltimateLyricsProvider::ReplaceField(const QString& tag,
                                          const QString& value,
                                          QString* text) const {
  if (!text->contains(tag)) return;

  // Apply URL character replacement
  QString value_copy(value);
  for (const UrlFormat& format : url_formats_) {
    QRegularExpression re("[" + QRegularExpression::escape(format.first) + "]");
    value_copy.replace(re, format.second);
  }

  text->replace(tag, value_copy, Qt::CaseInsensitive);
}

void UltimateLyricsProvider::ReplaceFields(const Song& metadata,
                                           QString* text) const {
  ReplaceField("{artist}", metadata.artist().toLower(), text);
  ReplaceField("{artist2}", NoSpace(metadata.artist().toLower()), text);
  ReplaceField("{album}", metadata.album().toLower(), text);
  ReplaceField("{album2}", NoSpace(metadata.album().toLower()), text);
  ReplaceField("{title}", metadata.title().toLower(), text);
  ReplaceField("{Artist}", metadata.artist(), text);
  ReplaceField("{Album}", metadata.album(), text);
  ReplaceField("{ARTIST}", metadata.artist().toUpper(), text);
  ReplaceField("{year}", metadata.PrettyYear(), text);
  ReplaceField("{Title}", metadata.title(), text);
  ReplaceField("{Title2}", TitleCase(metadata.title()), text);
  ReplaceField("{a}", FirstChar(metadata.artist()), text);
  ReplaceField("{A}", FirstChar(metadata.artist()).toUpper(), text);
  ReplaceField("{track}", QString::number(metadata.track()), text);
}

QString UltimateLyricsProvider::NoSpace(const QString& text) {
  QString ret(text);
  ret.remove(' ');
  return ret;
}

// tells whether a html block has alphanumeric characters (skipping tags)
// TODO: handle special characters (e.g. &reg; &aacute;)
bool UltimateLyricsProvider::HTMLHasAlphaNumeric(const QString& html) {
  bool in_tag = false;
  for (const QChar& c : html) {
    if (!in_tag and c.isLetterOrNumber())
      return true;
    else if (c == QChar('<'))
      in_tag = true;
    else if (c == QChar('>'))
      in_tag = false;
  }
  return false;
}
