/* This file is part of Clementine.
   Copyright 2012, David Sansome <me@davidsansome.com>

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

#include "moodbarrenderer.h"

#include <QObject>
#include <QPainter>
#include <QPalette>

#include "core/arraysize.h"

const int MoodbarRenderer::kNumHues = 12;

ColorVector MoodbarRenderer::Colors(const QByteArray& data,
                                    MoodbarStyle style,
                                    const QPalette& palette) {
  const int samples = data.size() / 3;

  // Keep thresholds at a minimum of 1 and avoid premature integer truncation.
  const auto threshold = [samples](int multiplier) {
    return qMax(1, (samples * multiplier) / 360);
  };

  StyleProperties properties;
  switch (style) {
    case Style_Angry:
      // Warm hue sweep: orange -> deep red.
      properties = StyleProperties(
          threshold(9),
          45,    // Range Start: Orange
          -45,   // Range Delta: Deep red
          200,   // Saturation
          100);  // Brightness
      break;

    case Style_Euphoric:
      // Hue sweep: magenta -> gold/yellow through warm hues.
      properties = StyleProperties(
          threshold(3),
          300,   // Range Start: Deep Magenta
          120,   // Range Delta: Warm sweep to Gold
          160,   // Saturation
          240);  // Brightness
      break;

    case Style_Frozen:
      // Cool hue sweep: teal -> blue -> violet/magenta.
      properties = StyleProperties(
          threshold(1),
          140,   // Range Start: Cool Teal
          160,   // Range Delta: Cool sweep
          50,    // Saturation
          100);  // Brightness
      break;

    case Style_Happy:
      // Full hue spectrum with elevated saturation and brightness.
      properties = StyleProperties(
          threshold(2),
          0,     // Range Start: Red
          359,   // Range Delta: Full spectrum
          150,   // Saturation
          250);  // Brightness
      break;

    case Style_Neon:
      // Neon hue sweep: electric cyan -> hot pink.
      properties = StyleProperties(
          threshold(4),
          180,   // Range Start: Electric Cyan
          140,   // Range Delta: Blue -> Violet -> Magenta -> Pink
          220,   // Saturation
          255);  // Full brightness
      break;

    case Style_Normal:
      // Balanced full hue spectrum.
      properties = StyleProperties(
          threshold(3),
          0,     // Range Start: Red
          359,   // Range Delta: Full spectrum
          100,   // Saturation
          100);  // Brightness
      break;

    case Style_SystemPalette:
    default: {
      const QColor highlight_color(
          palette.color(QPalette::Active, QPalette::Highlight));
      const int hue = highlight_color.hsvHue();

      properties = StyleProperties(
          threshold(3),
          (hue - 20 + 360) % 360,
          20,
          highlight_color.hsvSaturation(),
          highlight_color.value() / 2);
      break;
    }
  }

  const unsigned char* data_p =
      reinterpret_cast<const unsigned char*>(data.constData());

  int hue_distribution[360];
  int total = 0;

  memset(hue_distribution, 0, sizeof(hue_distribution));

  ColorVector colors;
  colors.reserve(samples);

  // Read the colors, keeping track of histograms.
  for (int i = 0; i < samples; ++i) {
    QColor color;
    color.setRed(int(*data_p++));
    color.setGreen(int(*data_p++));
    color.setBlue(int(*data_p++));

    colors << color;

    const int raw_hue = color.hue();
    const int hue = (raw_hue < 0) ? 0 : raw_hue;

    if (hue_distribution[hue]++ == properties.threshold_) {
      ++total;
    }
  }

  total = qMax(total, 1);

  // Remap hue values into the configured range.
  // Negative deltas are normalized back into the 0-359 hue range.
  for (int i = 0, n = 0; i < 360; ++i) {
    int mapped_hue =
        ((hue_distribution[i] > properties.threshold_ ? n++ : n) *
             properties.range_delta_ / total +
         properties.range_start_) %
        360;

    if (mapped_hue < 0) {
      mapped_hue += 360;
    }

    hue_distribution[i] = mapped_hue;
  }

  // hue_distribution is now a hue mapper:
  // hue_distribution[h] is the new hue value for a bar with hue h.
  for (ColorVector::iterator it = colors.begin(); it != colors.end(); ++it) {
    const int raw_hue = it->hue();
    const int hue = (raw_hue < 0) ? 0 : raw_hue;

    *it = QColor::fromHsv(
        qBound(0, hue_distribution[hue], 359),
        qBound(0, it->saturation() * properties.sat_ / 100, 255),
        qBound(0, it->value() * properties.val_ / 100, 255));
  }

  return colors;
}

void MoodbarRenderer::Render(const ColorVector& colors, QPainter* p,
                             const QRect& rect) {
  // Sample the colors and map them to screen pixels.
  ColorVector screen_colors;
  for (int x = 0; x < rect.width(); ++x) {
    int r = 0;
    int g = 0;
    int b = 0;

    int start = x * colors.size() / rect.width();
    int end = (x + 1) * colors.size() / rect.width();

    if (start == end) end = qMin(start + 1, colors.size() - 1);

    for (int j = start; j < end; j++) {
      r += colors[j].red();
      g += colors[j].green();
      b += colors[j].blue();
    }

    const int n = qMax(1, end - start);
    screen_colors.append(QColor(r / n, g / n, b / n));
  }

  // Draw the actual moodbar.
  for (int x = 0; x < rect.width(); x++) {
    int h, s, v;
    screen_colors[x].getHsv(&h, &s, &v);

    for (int y = 0; y <= rect.height() / 2; y++) {
      float coeff = float(y) / float(rect.height() / 2);
      float coeff2 = 1.0f - ((1.0f - coeff) * (1.0f - coeff));
      coeff = 1.0f - (1.0f - coeff) / 2.0f;
      coeff2 = 1.f - (1.f - coeff2) / 2.0f;

      p->setPen(QColor::fromHsv(
          h, qBound(0, int(float(s) * coeff), 255),
          qBound(0, int(255.f - (255.f - float(v)) * coeff2), 255)));

      p->drawPoint(rect.left() + x, rect.top() + y);
      p->drawPoint(rect.left() + x, rect.top() + rect.height() - 1 - y);
    }
  }
}

QImage MoodbarRenderer::RenderToImage(const ColorVector& colors,
                                      const QSize& size) {
  QImage image(size, QImage::Format_ARGB32_Premultiplied);
  QPainter p(&image);
  Render(colors, &p, image.rect());
  p.end();
  return image;
}

QString MoodbarRenderer::StyleName(MoodbarStyle style) {
  switch (style) {
    case Style_Normal:
      return QObject::tr("Normal");
    case Style_Angry:
      return QObject::tr("Angry");
    case Style_Euphoric:
      return QObject::tr("Euphoric");
    case Style_Frozen:
      return QObject::tr("Frozen");
    case Style_Happy:
      return QObject::tr("Happy");
    case Style_Neon:
      return QObject::tr("Neon");
    case Style_SystemPalette:
      return QObject::tr("System colors");

    default:
      return QString();
  }
}
