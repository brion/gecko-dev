/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsError.h"
#include "MediaDecoderStateMachine.h"
#include "AbstractMediaDecoder.h"
#include "MediaResource.h"
#include "OggDemuxer.h"
#include "OggCodecState.h"
#include "gfx2DGlue.h"
#include "mozilla/Preferences.h"
#include "mozilla/SharedThreadPool.h"
#include "mozilla/TimeStamp.h"
#include "MediaDataDemuxer.h"
#include "nsAutoRef.h"
#include "XiphExtradata.h"

#include <algorithm>
#include <stdint.h>

#define OGG_DEBUG(arg, ...) MOZ_LOG(gMediaDecoderLog, mozilla::LogLevel::Debug, ("OggDemuxer(%p)::%s: " arg, this, __func__, ##__VA_ARGS__))

namespace mozilla {

using namespace gfx;

extern PRLogModuleInfo* gMediaDecoderLog;

// Return the corresponding category in aKind based on the following specs.
// (https://www.whatwg.org/specs/web-apps/current-
// work/multipage/embedded-content.html#dom-audiotrack-kind) &
// (http://wiki.xiph.org/SkeletonHeaders)
const nsString
OggDemuxer::GetKind(const nsCString& aRole)
{
  if (aRole.Find("audio/main") != -1 || aRole.Find("video/main") != -1) {
    return NS_LITERAL_STRING("main");
  } else if (aRole.Find("audio/alternate") != -1 ||
             aRole.Find("video/alternate") != -1) {
    return NS_LITERAL_STRING("alternative");
  } else if (aRole.Find("audio/audiodesc") != -1) {
    return NS_LITERAL_STRING("descriptions");
  } else if (aRole.Find("audio/described") != -1) {
    return NS_LITERAL_STRING("main-desc");
  } else if (aRole.Find("audio/dub") != -1) {
    return NS_LITERAL_STRING("translation");
  } else if (aRole.Find("audio/commentary") != -1) {
    return NS_LITERAL_STRING("commentary");
  } else if (aRole.Find("video/sign") != -1) {
    return NS_LITERAL_STRING("sign");
  } else if (aRole.Find("video/captioned") != -1) {
    return NS_LITERAL_STRING("captions");
  } else if (aRole.Find("video/subtitled") != -1) {
    return NS_LITERAL_STRING("subtitles");
  }
  return EmptyString();
}

void
OggDemuxer::InitTrack(MessageField* aMsgInfo,
                      TrackInfo* aInfo,
                      bool aEnable)
{
  MOZ_ASSERT(aMsgInfo);
  MOZ_ASSERT(aInfo);

  nsCString* sName = aMsgInfo->mValuesStore.Get(eName);
  nsCString* sRole = aMsgInfo->mValuesStore.Get(eRole);
  nsCString* sTitle = aMsgInfo->mValuesStore.Get(eTitle);
  nsCString* sLanguage = aMsgInfo->mValuesStore.Get(eLanguage);
  aInfo->Init(sName? NS_ConvertUTF8toUTF16(*sName):EmptyString(),
              sRole? GetKind(*sRole):EmptyString(),
              sTitle? NS_ConvertUTF8toUTF16(*sTitle):EmptyString(),
              sLanguage? NS_ConvertUTF8toUTF16(*sLanguage):EmptyString(),
              aEnable);
}


OggDemuxer::OggDemuxer(MediaResource* aResource)
  : mTheoraState(nullptr),
    mVorbisState(nullptr),
    mOpusState(nullptr),
    mOpusEnabled(MediaDecoder::IsOpusEnabled()),
    mSkeletonState(nullptr),
    mVorbisSerial(0),
    mOpusSerial(0),
    mTheoraSerial(0),
    mOpusPreSkip(0),
    mIsChained(false),
    mDecodedAudioFrames(0),
    mResource(aResource),
    mHasAudio(false),
    mHasVideo(false)
{
  MOZ_COUNT_CTOR(OggDemuxer);
  memset(&mTheoraInfo, 0, sizeof(mTheoraInfo));
}

OggDemuxer::~OggDemuxer()
{
  Reset();
  Cleanup();
  MOZ_COUNT_DTOR(OggDemuxer);
}

RefPtr<OggDemuxer::InitPromise>
OggDemuxer::Init()
{
  int ret = ogg_sync_init(&mOggState);
  if (ret != 0) {
    return InitPromise::CreateAndReject(DemuxerFailureReason::DEMUXER_ERROR, __func__);
  }
  /*
  if (InitBufferedState() != NS_OK) {
    return InitPromise::CreateAndReject(DemuxerFailureReason::WAITING_FOR_DATA, __func__);
  }
  */
  if (ReadMetadata() != NS_OK) {
    return InitPromise::CreateAndReject(DemuxerFailureReason::DEMUXER_ERROR, __func__);
  }

  if (!GetNumberTracks(TrackInfo::kAudioTrack) &&
      !GetNumberTracks(TrackInfo::kVideoTrack)) {
    return InitPromise::CreateAndReject(DemuxerFailureReason::DEMUXER_ERROR, __func__);
  }

  return InitPromise::CreateAndResolve(NS_OK, __func__);
}

bool
OggDemuxer::HasTrackType(TrackInfo::TrackType aType) const
{
  return !!GetNumberTracks(aType);
}

OggCodecState *
OggDemuxer::GetTrackCodecState(TrackInfo::TrackType aType) const
{
  switch(aType) {
    case TrackInfo::kAudioTrack:
      if (mVorbisState) {
        return mVorbisState;
      } else {
        return mOpusState;
      }
    case TrackInfo::kVideoTrack:
      return mTheoraState;
    default:
      return 0;
  }
}

uint32_t
OggDemuxer::GetNumberTracks(TrackInfo::TrackType aType) const
{
  switch(aType) {
    case TrackInfo::kAudioTrack:
      return mHasAudio ? 1 : 0;
    case TrackInfo::kVideoTrack:
      return mHasVideo ? 1 : 0;
    default:
      return 0;
  }
}

UniquePtr<TrackInfo>
OggDemuxer::GetTrackInfo(TrackInfo::TrackType aType,
                          size_t aTrackNumber) const
{
  switch(aType) {
    case TrackInfo::kAudioTrack:
      return mInfo.mAudio.Clone();
    case TrackInfo::kVideoTrack:
      return mInfo.mVideo.Clone();
    default:
      return nullptr;
  }
}

already_AddRefed<MediaTrackDemuxer>
OggDemuxer::GetTrackDemuxer(TrackInfo::TrackType aType, uint32_t aTrackNumber)
{
  if (GetNumberTracks(aType) <= aTrackNumber) {
    return nullptr;
  }
  RefPtr<OggTrackDemuxer> e =
    new OggTrackDemuxer(this, aType, aTrackNumber);
  mDemuxers.AppendElement(e);

  return e.forget();
}

nsresult
OggDemuxer::Reset()
{
  MOZ_ASSERT(OnTaskQueue());
  nsresult res = NS_OK;

  // Discard any previously buffered packets/pages.
  ogg_sync_reset(&mOggState);
  if (mVorbisState && NS_FAILED(mVorbisState->Reset())) {
    res = NS_ERROR_FAILURE;
  }
  if (mOpusState && NS_FAILED(mOpusState->Reset())) { // false?
    res = NS_ERROR_FAILURE;
  }
  if (mTheoraState && NS_FAILED(mTheoraState->Reset())) {
    res = NS_ERROR_FAILURE;
  }

  return res;
}

nsresult
OggDemuxer::ResetTrackState(TrackInfo::TrackType aType)
{
  OggCodecState *trackState = GetTrackCodecState(aType);
  if (trackState) {
    trackState->Reset();
  }
}

void
OggDemuxer::Cleanup()
{
  ogg_sync_clear(&mOggState);

  // mBufferedState = nullptr; // ????
}

bool
OggDemuxer::ReadHeaders(OggCodecState* aState, MediaByteBuffer* aCodecSpecificConfig)
{
  nsAutoTArray<const unsigned char*,4> headers;
  nsAutoTArray<size_t,4> headerLens;
  while (!aState->DoneReadingHeaders()) {
    //ogg_packet* packet = aState->PacketOut(); // ?
    ogg_packet* packet = DemuxUntilPacketAvailable(aState);
    if (!packet) {
      OGG_DEBUG("Ran out of header packets early; deactivating stream %ld", aState->mSerial);
      aState->Deactivate();
      return false;
    }

    if (aCodecSpecificConfig) {
      // Save a copy of the header packet for the decoder to use later;
      // OggCodecState::DecodeHeader will free it when processing locally.
      size_t packetSize = packet->bytes;
      unsigned char *packetData = static_cast<unsigned char *>(malloc(packetSize));
      MOZ_ASSERT(packetData != nullptr, "Could not duplicate ogg header packet");
      memcpy(packetData, packet->packet, packetSize);

      headers.AppendElement(packetData);
      headerLens.AppendElement(packetSize);
    }

    // Local OggCodecState needs to decode headers in order to process
    // packet granulepos -> time mappings, etc.
    if (!aState->DecodeHeader(packet)) {
      OGG_DEBUG("Failed to decode ogg header packet; deactivating stream %ld", aState->mSerial);
      aState->Deactivate();
      return false;
    }
  }
  if (!XiphHeadersToExtradata(aCodecSpecificConfig, headers, headerLens)) {
    return false;
  }
  return aState->Init();
}

void
OggDemuxer::BuildSerialList(nsTArray<uint32_t>& aTracks)
{
  // Obtaining seek index information for currently active bitstreams.
  if (mHasVideo) {
    aTracks.AppendElement(mTheoraState->mSerial);
  }
  if (mHasAudio) {
    if (mVorbisState) {
      aTracks.AppendElement(mVorbisState->mSerial);
    } else if (mOpusState) {
      aTracks.AppendElement(mOpusState->mSerial);
    }
  }
}

bool
OggDemuxer::SetupTargetTheora()
{
  if (!ReadHeaders(mTheoraState, mInfo.mVideo.mCodecSpecificConfig)) {
    return false;
  }
  nsIntRect picture = nsIntRect(mTheoraState->mInfo.pic_x,
                                mTheoraState->mInfo.pic_y,
                                mTheoraState->mInfo.pic_width,
                                mTheoraState->mInfo.pic_height);

  nsIntSize displaySize = nsIntSize(mTheoraState->mInfo.pic_width,
                                    mTheoraState->mInfo.pic_height);

  // Apply the aspect ratio to produce the intrinsic display size we report
  // to the element.
  ScaleDisplayByAspectRatio(displaySize, mTheoraState->mPixelAspectRatio);

  nsIntSize frameSize(mTheoraState->mInfo.frame_width,
                      mTheoraState->mInfo.frame_height);
  if (IsValidVideoRegion(frameSize, picture, displaySize)) {
    // Video track's frame sizes will not overflow. Activate the video track.
    mInfo.mVideo.mMimeType = "video/ogg; codecs=theora";
    mInfo.mVideo.mDisplay = displaySize;
    mInfo.mVideo.mImage = picture;

    // @fixme set mInfo.mVideo.mDuration?

    // Copy Theora info data for time computations on other threads.
    memcpy(&mTheoraInfo, &mTheoraState->mInfo, sizeof(mTheoraInfo));
    return true;
  }
  return false;
}

bool
OggDemuxer::SetupTargetVorbis()
{
  if (!ReadHeaders(mVorbisState, mInfo.mAudio.mCodecSpecificConfig)) {
    return false;
  }
  
  // Copy Vorbis info data for time computations on other threads.
  memcpy(&mVorbisInfo, &mVorbisState->mInfo, sizeof(mVorbisInfo));
  mVorbisInfo.codec_setup = nullptr;
  
  mInfo.mAudio.mMimeType = "audio/ogg; codecs=vorbis";

  // @fixme duration?
  // @fixme mInfo.mAudio.mRate
  // @fixme mInfo.mAudio.mChannels

  return true;
}

bool
OggDemuxer::SetupTargetOpus()
{
  if (!ReadHeaders(mOpusState, mInfo.mAudio.mCodecSpecificConfig)) {
    return false;
  }
  mOpusPreSkip = mOpusState->mPreSkip;

  mInfo.mAudio.mMimeType = "audio/ogg; codecs=opus";

  // @fixme duration?
  // @fixme mInfo.mAudio.mRate
  // @fixme mInfo.mAudio.mChannels

  // @fixme codec-specific setup info
  //uint8_t c[sizeof(unit64_t)];
  //BigEndian::writeUint64(&c[0], mCodecDelay); 
  //mInfo.mAudio.mCodecSpecificConfig->AppendElements(&c[0], sizeof(uint64_t));

  return false; // not yet implemented fully
}

void
OggDemuxer::SetupTargetSkeleton()
{
  // Setup skeleton related information after mVorbisState & mTheroState
  // being set (if they exist).
  if (mSkeletonState) {
    if (!mHasAudio && !mHasVideo) {
      // We have a skeleton track, but no audio or video, may as well disable
      // the skeleton, we can't do anything useful with this media.
      OGG_DEBUG("Deactivating skeleton stream %ld", mSkeletonState->mSerial);
      mSkeletonState->Deactivate();
    } else if (ReadHeaders(mSkeletonState, nullptr) && mSkeletonState->HasIndex()) {
      // Extract the duration info out of the index, so we don't need to seek to
      // the end of resource to get it.
      nsAutoTArray<uint32_t, 2> tracks;
      BuildSerialList(tracks);
      int64_t duration = 0;
      if (NS_SUCCEEDED(mSkeletonState->GetDuration(tracks, duration))) {
        LOG(LogLevel::Debug, ("Got duration from Skeleton index %lld", duration));
        mInfo.mMetadataDuration.emplace(media::TimeUnit::FromMicroseconds(duration));
      }
    }
  }
}

void
OggDemuxer::SetupMediaTracksInfo(const nsTArray<uint32_t>& aSerials)
{
  // For each serial number
  // 1. Retrieve a codecState from mCodecStore by this serial number.
  // 2. Retrieve a message field from mMsgFieldStore by this serial number.
  // 3. For now, skip if the serial number refers to a non-primary bitstream.
  // 4. Setup track and other audio/video related information per different types.
  for (size_t i = 0; i < aSerials.Length(); i++) {
    uint32_t serial = aSerials[i];
    OggCodecState* codecState = mCodecStore.Get(serial);

    MessageField* msgInfo = nullptr;
    if (mSkeletonState && mSkeletonState->mMsgFieldStore.Contains(serial)) {
      mSkeletonState->mMsgFieldStore.Get(serial, &msgInfo);
    }

    if (codecState->GetType() == OggCodecState::TYPE_THEORA) {
      TheoraState* theoraState = static_cast<TheoraState*>(codecState);
      if (!(mTheoraState && mTheoraState->mSerial == theoraState->mSerial)) {
        continue;
      }

      if (msgInfo) {
        InitTrack(msgInfo,
                  &mInfo.mVideo,
                  mTheoraState == theoraState);
      }

      nsIntRect picture = nsIntRect(theoraState->mInfo.pic_x,
                                    theoraState->mInfo.pic_y,
                                    theoraState->mInfo.pic_width,
                                    theoraState->mInfo.pic_height);
      nsIntSize displaySize = nsIntSize(theoraState->mInfo.pic_width,
                                        theoraState->mInfo.pic_height);
      nsIntSize frameSize(theoraState->mInfo.frame_width,
                          theoraState->mInfo.frame_height);
      ScaleDisplayByAspectRatio(displaySize, theoraState->mPixelAspectRatio);
      if (IsValidVideoRegion(frameSize, picture, displaySize)) {
        mInfo.mVideo.mDisplay = displaySize;
      }
    } else if (codecState->GetType() == OggCodecState::TYPE_VORBIS) {
      VorbisState* vorbisState = static_cast<VorbisState*>(codecState);
      if (!(mVorbisState && mVorbisState->mSerial == vorbisState->mSerial)) {
        continue;
      }

      if (msgInfo) {
        InitTrack(msgInfo,
                  &mInfo.mAudio,
                  mVorbisState == vorbisState);
      }

      mInfo.mAudio.mRate = vorbisState->mInfo.rate;
      mInfo.mAudio.mChannels = vorbisState->mInfo.channels;
    } else if (codecState->GetType() == OggCodecState::TYPE_OPUS) {
      OpusState* opusState = static_cast<OpusState*>(codecState);
      if (!(mOpusState && mOpusState->mSerial == opusState->mSerial)) {
        continue;
      }

      if (msgInfo) {
        InitTrack(msgInfo,
                  &mInfo.mAudio,
                  mOpusState == opusState);
      }

      mInfo.mAudio.mRate = opusState->mRate;
      mInfo.mAudio.mChannels = opusState->mChannels;
    }
  }
}

nsresult
OggDemuxer::ReadMetadata()
{
  MOZ_ASSERT(OnTaskQueue());

  OGG_DEBUG("OggDemuxer::ReadMetadata called!");

  // We read packets until all bitstreams have read all their header packets.
  // We record the offset of the first non-header page so that we know
  // what page to seek to when seeking to the media start.

  // @FIXME we have to read all the header packets on all the streams
  // and THEN we can run SetupTarget*
  // @fixme fixme

  //NS_ASSERTION(aTags, "Called with null MetadataTags**.");
  //*aTags = nullptr;

  ogg_page page;
  nsAutoTArray<OggCodecState*,4> bitstreams;
  nsTArray<uint32_t> serials;
  bool readAllBOS = false;
  while (!readAllBOS) {
    if (!ReadOggPage(&page)) {
      // Some kind of error...
      OGG_DEBUG("OggDemuxer::ReadOggPage failed? leaving ReadMetadata...");
      break;
    }

    int serial = ogg_page_serialno(&page);

    if (!ogg_page_bos(&page)) {
      // We've encountered a non Beginning Of Stream page. No more BOS pages
      // can follow in this Ogg segment, so there will be no other bitstreams
      // in the Ogg (unless it's invalid).
      readAllBOS = true;
    } else if (!mCodecStore.Contains(serial)) {
      // We've not encountered a stream with this serial number before. Create
      // an OggCodecState to demux it, and map that to the OggCodecState
      // in mCodecStates.
      OggCodecState* codecState = OggCodecState::Create(&page);
      mCodecStore.Add(serial, codecState);
      bitstreams.AppendElement(codecState);
      serials.AppendElement(serial);
    }
    if (NS_FAILED(DemuxOggPage(&page))) {
      return NS_ERROR_FAILURE;
    }
  }

  // We've read all BOS pages, so we know the streams contained in the media.
  // 1. Find the first encountered Theora/Vorbis/Opus bitstream, and configure
  //    it as the target A/V bitstream.
  // 2. Deactivate the rest of bitstreams for now, until we have MediaInfo
  //    support multiple track infos.
  for (uint32_t i = 0; i < bitstreams.Length(); ++i) {
    OggCodecState* s = bitstreams[i];
    if (s) {
      if (s->GetType() == OggCodecState::TYPE_THEORA) {
        if (!mHasVideo) {
          mTheoraSerial = s->mSerial;
          mTheoraState = static_cast<TheoraState*>(s);
          mHasVideo = true;
        } else {
          OGG_DEBUG("Deactivating extra Theora stream %ld", s->mSerial);
          s->Deactivate();
        }
      } else if (s->GetType() == OggCodecState::TYPE_VORBIS) {
        if (!mHasAudio) {
          mVorbisSerial = s->mSerial;
          mVorbisState = static_cast<VorbisState*>(s);
          mHasAudio = true;
        } else {
          OGG_DEBUG("Deactivating extra Vorbis stream %ld", s->mSerial);
          s->Deactivate();
        }
      } else if (s->GetType() == OggCodecState::TYPE_OPUS /*&& ReadHeaders(s) whaaaat?*/) {
        if (mOpusEnabled) {
          if (!mHasAudio) {
            mOpusSerial = s->mSerial;
            mOpusState = static_cast<OpusState*>(s);
            mHasAudio = true;
          } else {
            OGG_DEBUG("Deactivating extra Opus stream %ld", s->mSerial);
            s->Deactivate();
          }
        } else {
          NS_WARNING("Opus decoding disabled."
                     " See media.opus.enabled in about:config");
        }
      } else if (s->GetType() == OggCodecState::TYPE_SKELETON && !mSkeletonState) {
        mSkeletonState = static_cast<SkeletonState*>(s);
      } else {
        // Deactivate any non-primary bitstreams.
        OGG_DEBUG("Deactivating mystery stream %ld", s->mSerial);
        s->Deactivate();
      }

    }
  }

  // 3. Process all available header packets in the Theora, Vorbis/Opus bitstreams.
  if (mTheoraState) {
    if (SetupTargetTheora()) {
      OGG_DEBUG("Set up Theora stream %ld", mTheoraState->mSerial);
    } else {
      OGG_DEBUG("Deactivating Theora stream %ld", mTheoraState->mSerial);
      mTheoraState->Deactivate();
      mTheoraState->Reset();
      mTheoraState = nullptr;
      mTheoraSerial = 0;
      mHasVideo = false;
    }
  }
  if (mVorbisState) {
    if (SetupTargetVorbis()) {
      OGG_DEBUG("Set up Vorbis stream %ld", mVorbisState->mSerial);
    } else {
      OGG_DEBUG("Deactivating Vorbis stream %ld", mVorbisState->mSerial);
      mVorbisState->Deactivate();
      mVorbisState->Reset();
      mVorbisState = nullptr;
      mVorbisSerial = 0;
      mHasAudio = false;
    }
  }
  if (mOpusState) {
    if (SetupTargetOpus()) {
      OGG_DEBUG("Set up Opus stream %ld", mOpusState->mSerial);
    } else {
      OGG_DEBUG("Deactivating Opus stream %ld", mOpusState->mSerial);
      mOpusState->Deactivate();
      mOpusState->Reset();
      mOpusState = nullptr;
      mOpusSerial = 0;
      mHasAudio = false;
    }
  }

  SetupTargetSkeleton();
  SetupMediaTracksInfo(serials);

  if (mHasAudio || mHasVideo) {
    //ReentrantMonitorAutoEnter mon(mDecoder->GetReentrantMonitor());

    if (mInfo.mMetadataDuration.isNothing() /*&& !mDecoder->IsOggDecoderShutdown()*/ &&
        mResource.GetLength() >= 0 && IsSeekable())
    {
      // We didn't get a duration from the index or a Content-Duration header.
      // Seek to the end of file to find the end time.
      int64_t length = mResource.GetLength();

      NS_ASSERTION(length > 0, "Must have a content length to get end time");

      int64_t endTime = 0;
      {
        //ReentrantMonitorAutoExit exitMon(mDecoder->GetReentrantMonitor());
        // @fixme copy/rework this logic from OggReader?
        endTime = RangeEndTime(length);
      }
      if (endTime != -1) {
        mInfo.mUnadjustedMetadataEndTime.emplace(media::TimeUnit::FromMicroseconds(endTime));
        OGG_DEBUG("Got Ogg duration from seeking to end %lld", endTime);
      }
    }
  } else {
    OGG_DEBUG("no audio or video tracks");
    return NS_ERROR_FAILURE;
  }

  OGG_DEBUG("success?!");
  return NS_OK;
}

bool
OggDemuxer::ReadOggPage(ogg_page* aPage)
{
  int ret = 0;
  while((ret = ogg_sync_pageseek(&mOggState, aPage)) <= 0) {
    if (ret < 0) {
      // Lost page sync, have to skip up to next page.
      continue;
    }
    // Returns a buffer that can be written too
    // with the given size. This buffer is stored
    // in the ogg synchronisation structure.
    char* buffer = ogg_sync_buffer(&mOggState, 4096);
    NS_ASSERTION(buffer, "ogg_sync_buffer failed");

    // Read from the resource into the buffer
    uint32_t bytesRead = 0;

    nsresult rv = mResource.Read(buffer, 4096, &bytesRead);
    if (NS_FAILED(rv) || !bytesRead) {
      // End of file or error.
      return false;
    }

    // Update the synchronisation layer with the number
    // of bytes written to the buffer
    ret = ogg_sync_wrote(&mOggState, bytesRead);
    NS_ENSURE_TRUE(ret == 0, false);
  }

  return true;
}

nsresult
OggDemuxer::DemuxOggPage(ogg_page* aPage)
{
  int serial = ogg_page_serialno(aPage);
  OggCodecState* codecState = mCodecStore.Get(serial);
  if (codecState == nullptr) {
    OGG_DEBUG("encountered packet for unrecognized codecState");
    return NS_ERROR_FAILURE;
  }
  if (NS_FAILED(codecState->PageIn(aPage))) {
    OGG_DEBUG("codecState->PageIn failed");
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

bool
OggDemuxer::IsSeekable() const
{
  if (mIsChained) {
    return false;
  }
  return true;
}

UniquePtr<EncryptionInfo>
OggDemuxer::GetCrypto()
{
  return nullptr;
}

RefPtr<MediaRawData>
OggDemuxer::GetNextPacket(TrackInfo::TrackType aType)
{
  int r = 0;
  
  OggCodecState *state = GetTrackCodecState(aType);
  //ogg_packet *pkt = state->PacketOut();
  ogg_packet *pkt = DemuxUntilPacketAvailable(state);

  //if (!pkt) {
  //  return nullptr;
  //}

  unsigned char* data = pkt->packet;
  size_t length = pkt->bytes;

  // granulepos is end time
  int64_t end_tstamp = state->Time(pkt->granulepos);
  int64_t start_tstamp = state->StartTime(pkt->granulepos);

  bool isKeyframe = false;
  if (aType == TrackInfo::kAudioTrack) {
    isKeyframe = true;
  } else if (aType == TrackInfo::kVideoTrack) {
    isKeyframe = isTheoraKeyframe(pkt);
  }

  OGG_DEBUG("push sample start_tstamp: %ld end_tstamp: %ld length: %ld kf: %d",
             start_tstamp, end_tstamp, length, isKeyframe);
  RefPtr<MediaRawData> sample = new MediaRawData(data, length);
  sample->mTimecode = pkt->granulepos;
  sample->mTime = start_tstamp;
  sample->mDuration = end_tstamp - start_tstamp;
  sample->mOffset = 0; //???
  sample->mKeyframe = isKeyframe;

  return sample;
}

ogg_packet *
OggDemuxer::DemuxUntilPacketAvailable(OggCodecState *state)
{
  ogg_packet *pkt = state->PacketOut();
  while (pkt == nullptr) {
    OGG_DEBUG("no packet yet, reading some more");
    ogg_page page;
    if (!ReadOggPage(&page)) {
      OGG_DEBUG("no more pages to read in resource?");
      return nullptr;
    }
    DemuxOggPage(&page);
    pkt = state->PacketOut();
  }
  if (pkt) {
    OGG_DEBUG("got a packet");
  } else {
    OGG_DEBUG("don't got a packet");
  }
  return pkt;
}

media::TimeIntervals
OggDemuxer::GetBuffered()
{
  //EnsureUpToDateIndex();
  AutoPinned<MediaResource> resource(mResource.GetResource());

  media::TimeIntervals buffered;

  nsTArray<MediaByteRange> ranges;
  nsresult rv = resource->GetCachedRanges(ranges);
  if (NS_FAILED(rv)) {
    return media::TimeIntervals();
  }
  uint64_t duration = 0;
  uint64_t startOffset = 0;
  /*
  if (!nestegg_duration(mContext, &duration)) {
    if(mBufferedState->GetStartTime(&startOffset)) {
      duration += startOffset;
    }
    OGG_DEBUG("Duration: %f StartTime: %f",
               media::TimeUnit::FromNanoseconds(duration).ToSeconds(),
               media::TimeUnit::FromNanoseconds(startOffset).ToSeconds());
  }
  for (uint32_t index = 0; index < ranges.Length(); index++) {
    uint64_t start, end;
    bool rv = mBufferedState->CalculateBufferedForRange(ranges[index].mStart,
                                                        ranges[index].mEnd,
                                                        &start, &end);
    if (rv) {
      NS_ASSERTION(startOffset <= start,
          "startOffset negative or larger than start time");

      if (duration && end > duration) {
        OGG_DEBUG("limit range to duration, end: %f duration: %f",
                   media::TimeUnit::FromNanoseconds(end).ToSeconds(),
                   media::TimeUnit::FromNanoseconds(duration).ToSeconds());
        end = duration;
      }
      media::TimeUnit startTime = media::TimeUnit::FromNanoseconds(start);
      media::TimeUnit endTime = media::TimeUnit::FromNanoseconds(end);
      OGG_DEBUG("add range %f-%f", startTime.ToSeconds(), endTime.ToSeconds());
      buffered += media::TimeInterval(startTime, endTime);
    }
  }
  */
  return buffered; // @fixme
}

nsresult
OggDemuxer::SeekInternal(const media::TimeUnit& aTarget)
{
  return NS_ERROR_NOT_IMPLEMENTED; // @fixme
}

bool
OggDemuxer::GetOffsetForTime(uint64_t aTime, int64_t* aOffset)
{
  //EnsureUpToDateIndex();
  //return mBufferedState && mBufferedState->GetOffsetForTime(aTime, aOffset);
  return false; // @fixme
}

bool
OggDemuxer::isTheoraKeyframe(ogg_packet* pkt)
{
  // first bit of packet is 1 for header, 0 for data
  // second bit of packet is 1 for inter frame, 0 for intra frame
  return (pkt->bytes >= 1 && (pkt->packet[0] & 0x40) == 0x00);
}


//OggTrackDemuxer
OggTrackDemuxer::OggTrackDemuxer(OggDemuxer* aParent,
                                   TrackInfo::TrackType aType,
                                   uint32_t aTrackNumber)
  : mParent(aParent)
  , mType(aType)
{
  mInfo = mParent->GetTrackInfo(aType, aTrackNumber);
  MOZ_ASSERT(mInfo);
}

OggTrackDemuxer::~OggTrackDemuxer()
{
}

UniquePtr<TrackInfo>
OggTrackDemuxer::GetInfo() const
{
  return mInfo->Clone();
}

RefPtr<OggTrackDemuxer::SeekPromise>
OggTrackDemuxer::Seek(media::TimeUnit aTime)
{
  // Seeks to aTime. Upon success, SeekPromise will be resolved with the
  // actual time seeked to. Typically the random access point time

  media::TimeUnit seekTime = aTime;
  mParent->SeekInternal(aTime);
  RefPtr<MediaRawData> sample(NextSample());

  // Check what time we actually seeked to.
  seekTime = media::TimeUnit::FromMicroseconds(sample->mTime);
  mQueuedSample = sample;

  return SeekPromise::CreateAndResolve(seekTime, __func__);
}

RefPtr<MediaRawData>
OggTrackDemuxer::NextSample()
{
  RefPtr<MediaRawData> nextSample;
  if (mQueuedSample) {
    nextSample = mQueuedSample;
  } else {
    nextSample = mParent->GetNextPacket(mType);
  }
  mQueuedSample = mParent->GetNextPacket(mType);
  return nextSample;
}

RefPtr<OggTrackDemuxer::SamplesPromise>
OggTrackDemuxer::GetSamples(int32_t aNumSamples)
{
  RefPtr<SamplesHolder> samples = new SamplesHolder;
  if (!aNumSamples) {
    return SamplesPromise::CreateAndReject(DemuxerFailureReason::DEMUXER_ERROR, __func__);
  }

  while (aNumSamples) {
    RefPtr<MediaRawData> sample(NextSample());
    if (!sample) {
      break;
    }
    samples->mSamples.AppendElement(sample);
    aNumSamples--;
  }

  if (samples->mSamples.IsEmpty()) {
    return SamplesPromise::CreateAndReject(DemuxerFailureReason::END_OF_STREAM, __func__);
  } else {
    return SamplesPromise::CreateAndResolve(samples, __func__);
  }
}

void
OggTrackDemuxer::Reset()
{
  mParent->ResetTrackState(mType);
  media::TimeIntervals buffered = GetBuffered();
  if (buffered.Length()) {
    OGG_DEBUG("Seek to start point: %f", buffered.Start(0).ToSeconds());
    mParent->SeekInternal(buffered.Start(0));
  }
}

RefPtr<OggTrackDemuxer::SkipAccessPointPromise>
OggTrackDemuxer::SkipToNextRandomAccessPoint(media::TimeUnit aTimeThreshold)
{
  uint32_t parsed = 0;
  bool found = false;
  RefPtr<MediaRawData> sample;

  OGG_DEBUG("TimeThreshold: %f", aTimeThreshold.ToSeconds());
  while (!found && (sample = NextSample())) {
    parsed++;
    if (sample->mKeyframe && sample->mTime >= aTimeThreshold.ToMicroseconds()) {
      found = true;
      mQueuedSample = sample;
    }
  }
  if (found) {
    OGG_DEBUG("next sample: %f (parsed: %d)",
               media::TimeUnit::FromMicroseconds(sample->mTime).ToSeconds(),
               parsed);
    return SkipAccessPointPromise::CreateAndResolve(parsed, __func__);
  } else {
    SkipFailureHolder failure(DemuxerFailureReason::END_OF_STREAM, parsed);
    return SkipAccessPointPromise::CreateAndReject(Move(failure), __func__);
  }
}

media::TimeIntervals
OggTrackDemuxer::GetBuffered()
{
  return mParent->GetBuffered();
}

void
OggTrackDemuxer::BreakCycles()
{
  mParent = nullptr;
}


// Returns an ogg page's checksum.
ogg_uint32_t
OggDemuxer::GetPageChecksum(ogg_page* page)
{
  if (page == 0 || page->header == 0 || page->header_len < 25) {
    return 0;
  }
  const unsigned char* p = page->header + 22;
  uint32_t c =  p[0] +
               (p[1] << 8) +
               (p[2] << 16) +
               (p[3] << 24);
  return c;
}

int64_t
OggDemuxer::RangeStartTime(int64_t aOffset)
{
  MOZ_ASSERT(OnTaskQueue());
  nsresult res = mResource.Seek(nsISeekableStream::NS_SEEK_SET, aOffset);
  NS_ENSURE_SUCCESS(res, 0);
  int64_t startTime = 0;
  //FindStartTime(startTime); // @fixme
  return startTime;
}

struct nsDemuxerAutoOggSyncState {
  nsDemuxerAutoOggSyncState() {
    ogg_sync_init(&mState);
  }
  ~nsDemuxerAutoOggSyncState() {
    ogg_sync_clear(&mState);
  }
  ogg_sync_state mState;
};

int64_t
OggDemuxer::RangeEndTime(int64_t aEndOffset)
{
  MOZ_ASSERT(OnTaskQueue());

  int64_t position = mResource.Tell();
  int64_t endTime = RangeEndTime(0, aEndOffset, false);
  nsresult res = mResource.Seek(nsISeekableStream::NS_SEEK_SET, position);
  NS_ENSURE_SUCCESS(res, -1);
  return endTime;
}

int64_t
OggDemuxer::RangeEndTime(int64_t aStartOffset,
                         int64_t aEndOffset,
                         bool aCachedDataOnly)
{
  nsDemuxerAutoOggSyncState sync;

  // We need to find the last page which ends before aEndOffset that
  // has a granulepos that we can convert to a timestamp. We do this by
  // backing off from aEndOffset until we encounter a page on which we can
  // interpret the granulepos. If while backing off we encounter a page which
  // we've previously encountered before, we'll either backoff again if we
  // haven't found an end time yet, or return the last end time found.
  const int step = 5000;
  const int maxOggPageSize = 65306;
  int64_t readStartOffset = aEndOffset;
  int64_t readLimitOffset = aEndOffset;
  int64_t readHead = aEndOffset;
  int64_t endTime = -1;
  uint32_t checksumAfterSeek = 0;
  uint32_t prevChecksumAfterSeek = 0;
  bool mustBackOff = false;
  while (true) {
    OGG_DEBUG("%lld @ %lld-%lld ; time %lld", readHead, readStartOffset, readLimitOffset, endTime);
    ogg_page page;
    int ret = ogg_sync_pageseek(&sync.mState, &page);
    if (ret == 0) {
      // We need more data if we've not encountered a page we've seen before,
      // or we've read to the end of file.
      if (mustBackOff || readHead == aEndOffset || readHead == aStartOffset) {
        if (endTime != -1 || readStartOffset == 0) {
          // We have encountered a page before, or we're at the end of file.
          OGG_DEBUG("end of file or confused");
          break;
        }
        mustBackOff = false;
        prevChecksumAfterSeek = checksumAfterSeek;
        checksumAfterSeek = 0;
        ogg_sync_reset(&sync.mState);
        readStartOffset = std::max(static_cast<int64_t>(0), readStartOffset - step);
        // There's no point reading more than the maximum size of
        // an Ogg page into data we've previously scanned. Any data
        // between readLimitOffset and aEndOffset must be garbage
        // and we can ignore it thereafter.
        readLimitOffset = std::min(readLimitOffset,
                                 readStartOffset + maxOggPageSize);
        readHead = std::max(aStartOffset, readStartOffset);
      }

      int64_t limit = std::min(static_cast<int64_t>(UINT32_MAX),
                             aEndOffset - readHead);
      limit = std::max(static_cast<int64_t>(0), limit);
      limit = std::min(limit, static_cast<int64_t>(step));
      uint32_t bytesToRead = static_cast<uint32_t>(limit);
      uint32_t bytesRead = 0;
      char* buffer = ogg_sync_buffer(&sync.mState, bytesToRead);
      NS_ASSERTION(buffer, "Must have buffer");
      nsresult res;
      OGG_DEBUG("reading %ld bytes starting at %lld", bytesToRead, readHead);
      if (aCachedDataOnly) {
        res = mResource.GetResource()->ReadFromCache(buffer, readHead, bytesToRead);
        NS_ENSURE_SUCCESS(res, -1);
        bytesRead = bytesToRead;
      } else {
        NS_ASSERTION(readHead < aEndOffset,
                     "resource pos must be before range end");
        res = mResource.Seek(nsISeekableStream::NS_SEEK_SET, readHead);
        NS_ENSURE_SUCCESS(res, -1);
        res = mResource.Read(buffer, bytesToRead, &bytesRead);
        NS_ENSURE_SUCCESS(res, -1);
      }
      readHead += bytesRead;
      if (readHead > readLimitOffset) {
        mustBackOff = true;
      }

      // Update the synchronisation layer with the number
      // of bytes written to the buffer
      ret = ogg_sync_wrote(&sync.mState, bytesRead);
      if (ret != 0) {
        endTime = -1;
        OGG_DEBUG("failed right at the end, confused");
        break;
      }
      OGG_DEBUG("continuing after first check");
      continue;
    }

    if (ret < 0 || ogg_page_granulepos(&page) < 0) {
      OGG_DEBUG("continuing because something failed on a page?");
      continue;
    }

    uint32_t checksum = GetPageChecksum(&page);
    if (checksumAfterSeek == 0) {
      // This is the first page we've decoded after a backoff/seek. Remember
      // the page checksum. If we backoff further and encounter this page
      // again, we'll know that we won't find a page with an end time after
      // this one, so we'll know to back off again.
      checksumAfterSeek = checksum;
    }
    if (checksum == prevChecksumAfterSeek) {
      // This page has the same checksum as the first page we encountered
      // after the last backoff/seek. Since we've already scanned after this
      // page and failed to find an end time, we may as well backoff again and
      // try to find an end time from an earlier page.
      mustBackOff = true;
      OGG_DEBUG("continuing because we didn't back up far enough");
      continue;
    }

    int64_t granulepos = ogg_page_granulepos(&page);
    int serial = ogg_page_serialno(&page);

    OggCodecState* codecState = nullptr;
    codecState = mCodecStore.Get(serial);
    OGG_DEBUG("got codec state for serial %d", serial);
    if (!codecState) {
      // This page is from a bitstream which we haven't encountered yet.
      // It's probably from a new "link" in a "chained" ogg. Don't
      // bother even trying to find a duration...
      //SetChained(true); // @fixme fix this :D
      endTime = -1;
      OGG_DEBUG("breaking because no matching codec state");
      break;
    }

    int64_t t = codecState->Time(granulepos);
    OGG_DEBUG("got time %lld", t);
    if (t != -1) {
      endTime = t;
    }
  }

  return endTime;
}


#undef OGG_DEBUG
} // namespace mozilla
