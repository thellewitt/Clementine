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

#include "streamdiscoverer.h"

#include <gst/pbutils/pbutils.h>

#include <QEventLoop>

#include "core/logging.h"
#include "core/signalchecker.h"
#include "core/waitforsignal.h"

const int StreamDiscoverer::kDiscoveryTimeoutS = 10;

StreamDiscoverer::StreamDiscoverer() : QObject(nullptr) {
  // Setting up a discoverer:
  discoverer_ = gst_discoverer_new(kDiscoveryTimeoutS * GST_SECOND, NULL);
  if (discoverer_ == NULL) {
    qLog(Error) << "Error creating discoverer";
    return;
  }

  // Connecting its signals:
  CHECKED_GCONNECT(discoverer_, "discovered", &OnDiscovered, this);
  CHECKED_GCONNECT(discoverer_, "finished", &OnFinished, this);
  CHECKED_GCONNECT(discoverer_, "source-setup", &OnSourceSetup, this);

  // Starting the discoverer process:
  gst_discoverer_start(discoverer_);
}

StreamDiscoverer::~StreamDiscoverer() {
  gst_discoverer_stop(discoverer_);
  g_object_unref(discoverer_);
}

void StreamDiscoverer::Discover(const QString& url,
                                const QByteArray& auth_header) {
  auth_header_ = auth_header;

  qLog(Debug) << "Discover" << url;

  if (!gst_discoverer_discover_uri_async(discoverer_,
                                         url.toStdString().c_str())) {
    qLog(Error) << "Failed to start discovering" << url;
    auth_header_.clear();
    return;
  }

  WaitForSignal(this, SIGNAL(DiscoverFinished()));
  auth_header_.clear();
}

void StreamDiscoverer::OnDiscovered(GstDiscoverer* discoverer,
                                    GstDiscovererInfo* info, GError* err,
                                    gpointer self) {
  StreamDiscoverer* instance = reinterpret_cast<StreamDiscoverer*>(self);

  QString discovered_url(gst_discoverer_info_get_uri(info));

  GstDiscovererResult result = gst_discoverer_info_get_result(info);
  if (result != GST_DISCOVERER_OK) {
    const QString error_detail =
        err ? QString::fromUtf8(err->message)
            : tr("No GStreamer error details supplied");

    const char* domain = err ? g_quark_to_string(err->domain) : nullptr;

    qLog(Error) << "Discovery failed:"
                << "URI" << discovered_url
                << "result" << static_cast<int>(result)
                << "domain" << (domain ? domain : "(none)")
                << "code" << (err ? err->code : -1)
                << "message" << error_detail;

    QString error_message = GSTdiscovererErrorMessage(result);
    emit instance->Error(
        tr("Error discovering %1: %2").arg(discovered_url).arg(error_message));
    return;
  }

  // Get audio streams (we will only care about the first one, which should be
  // the only one).
  GList* audio_streams = gst_discoverer_info_get_audio_streams(info);

  if (audio_streams != nullptr) {
    qLog(Debug) << "Discovery successful";
    // We found a valid audio stream, extracting and saving its info:
    GstDiscovererStreamInfo* stream_audio_info =
        (GstDiscovererStreamInfo*)g_list_first(audio_streams)->data;

    StreamDetails stream_details;
    stream_details.url = discovered_url;
    stream_details.bitrate = gst_discoverer_audio_info_get_bitrate(
        GST_DISCOVERER_AUDIO_INFO(stream_audio_info));
    stream_details.channels = gst_discoverer_audio_info_get_channels(
        GST_DISCOVERER_AUDIO_INFO(stream_audio_info));
    stream_details.depth = gst_discoverer_audio_info_get_depth(
        GST_DISCOVERER_AUDIO_INFO(stream_audio_info));
    stream_details.sample_rate = gst_discoverer_audio_info_get_sample_rate(
        GST_DISCOVERER_AUDIO_INFO(stream_audio_info));

    // Human-readable codec name:
    GstCaps* stream_caps =
        gst_discoverer_stream_info_get_caps(stream_audio_info);
    gchar* decoder_description =
        gst_pb_utils_get_codec_description(stream_caps);
    stream_details.format = (decoder_description == NULL)
                                ? QString(tr("Unknown"))
                                : QString(decoder_description);

    gst_caps_unref(stream_caps);
    g_free(decoder_description);

    emit instance->DataReady(stream_details);

  } else {
    emit instance->Error(
        tr("Could not detect an audio stream in %1").arg(discovered_url));
  }

  gst_discoverer_stream_info_list_free(audio_streams);
}

void StreamDiscoverer::OnFinished(GstDiscoverer* discoverer, gpointer self) {
  // The discoverer doesn't have any more urls in its queue. Let the loop know
  // it can exit.
  StreamDiscoverer* instance = reinterpret_cast<StreamDiscoverer*>(self);
  emit instance->DiscoverFinished();
}

QString StreamDiscoverer::GSTdiscovererErrorMessage(
    GstDiscovererResult result) {
  switch (result) {
    case (GST_DISCOVERER_URI_INVALID):
      return tr("Invalid URL");
    case (GST_DISCOVERER_TIMEOUT):
      return tr("Connection timed out");
    case (GST_DISCOVERER_BUSY):
      return tr("The discoverer is busy");
    case (GST_DISCOVERER_MISSING_PLUGINS):
      return tr("Missing plugins");
    case (GST_DISCOVERER_ERROR):
    default:
      return tr("Could not get details");
  }
}

void StreamDiscoverer::OnSourceSetup(GstDiscoverer* discoverer,
                                     GstElement* source, gpointer self) {
  StreamDiscoverer* instance =
      reinterpret_cast<StreamDiscoverer*>(self);

  if (instance->auth_header_.isEmpty()) return;

  if (!g_object_class_find_property(G_OBJECT_GET_CLASS(source),
                                    "extra-headers")) {
    qLog(Debug) << "GStreamer source does not support extra HTTP headers:"
                << GST_ELEMENT_NAME(source);
    return;
  }

  GstStructure* headers = gst_structure_new(
      "headers", "Authorization", G_TYPE_STRING,
      instance->auth_header_.constData(), nullptr);

  g_object_set(source, "extra-headers", headers, nullptr);
  gst_structure_free(headers);

  qLog(Debug) << "Configured authenticated HTTP source for discovery";
}
