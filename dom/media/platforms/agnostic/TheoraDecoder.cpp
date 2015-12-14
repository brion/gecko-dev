/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TheoraDecoder.h"
#include "XiphExtradata.h"
#include "gfx2DGlue.h"
#include "nsError.h"
#include "TimeUnits.h"
#include "mozilla/PodOperations.h"

#include <algorithm>

#undef LOG
#define LOG(arg, ...) MOZ_LOG(gMediaDecoderLog, mozilla::LogLevel::Debug, ("TheoraDecoder(%p)::%s: " arg, this, __func__, ##__VA_ARGS__))

namespace mozilla {

using namespace gfx;
using namespace layers;

extern LazyLogModule gMediaDecoderLog;

ogg_packet InitTheoraPacket(const unsigned char* aData, size_t aLength,
                         bool aBOS, bool aEOS,
                         int64_t aGranulepos, int64_t aPacketNo)
{
  ogg_packet packet;
  packet.packet = const_cast<unsigned char*>(aData);
  packet.bytes = aLength;
  packet.b_o_s = aBOS;
  packet.e_o_s = aEOS;
  packet.granulepos = aGranulepos;
  packet.packetno = aPacketNo;
  return packet;
}

TheoraDecoder::TheoraDecoder(const VideoInfo& aConfig,
                       ImageContainer* aImageContainer,
                       FlushableTaskQueue* aTaskQueue,
                       MediaDataDecoderCallback* aCallback)
  : mImageContainer(aImageContainer)
  , mTaskQueue(aTaskQueue)
  , mCallback(aCallback)
  , mInfo(aConfig)
{
  MOZ_COUNT_CTOR(TheoraDecoder);
  mTheoraSetupInfo = nullptr;
  mTheoraDecoderContext = nullptr;
  mTheoraHeaders = 0;
  mPacketCount = 0;
}

TheoraDecoder::~TheoraDecoder()
{
  MOZ_COUNT_DTOR(TheoraDecoder);
}

nsresult
TheoraDecoder::Shutdown()
{
  if (mTheoraDecoderContext) {
    th_decode_free(mTheoraDecoderContext);
    mTheoraSetupInfo = nullptr;
    mTheoraDecoderContext = nullptr;
  }
  return NS_OK;
}

RefPtr<MediaDataDecoder::InitPromise>
TheoraDecoder::Init()
{
  th_comment_init(&mTheoraComment);
  th_info_init(&mTheoraInfo);

  nsAutoTArray<unsigned char*, 4> headers;
  nsAutoTArray<size_t, 4> headerLens;
  if (!XiphExtradataToHeaders(headers, headerLens,
	mInfo.mCodecSpecificConfig->Elements(),
	mInfo.mCodecSpecificConfig->Length())) {
	return InitPromise::CreateAndReject(DecoderFailureReason::INIT_ERROR, __func__);
  }
  for (size_t i = 0; i < headers.Length(); i++) {
    if (NS_FAILED(DecodeHeader(headers[i], headerLens[i]))) {
      return InitPromise::CreateAndReject(DecoderFailureReason::INIT_ERROR, __func__);
    }
  }

  MOZ_ASSERT(mPacketCount == 3);

  return InitPromise::CreateAndResolve(TrackInfo::kVideoTrack, __func__);
}

nsresult
TheoraDecoder::Flush()
{
  mTaskQueue->Flush();
  return NS_OK;
}

nsresult
TheoraDecoder::DecodeHeader(const unsigned char* aData, size_t aLength)
{
  bool bos = mPacketCount == 0;
  ogg_packet pkt = InitTheoraPacket(aData, aLength, bos, false, 0, mPacketCount++);

  int r = th_decode_headerin(&mTheoraInfo,
                             &mTheoraComment,
                             &mTheoraSetupInfo,
                             &pkt);
  return r > 0 ? NS_OK : NS_ERROR_FAILURE;
}

int
TheoraDecoder::DoDecodeFrame(MediaRawData* aSample)
{
#if defined(DEBUG)
  // ...
#endif

  const unsigned char* aData = aSample->Data();
  size_t aLength = aSample->Size();

  bool bos = mPacketCount == 0;
  ogg_packet pkt = InitTheoraPacket(aData, aLength, bos, false, aSample->mTimecode, mPacketCount++);

  int ret = th_decode_packetin(mTheoraDecoderContext, &pkt, nullptr);
  if (ret == 0 || ret == TH_DUPFRAME) {
    th_ycbcr_buffer ycbcr;
    th_decode_ycbcr_out(mTheoraDecoderContext, ycbcr);

    int hdec = !(mTheoraInfo.pixel_fmt & 1);
    int vdec = !(mTheoraInfo.pixel_fmt & 2);

    VideoData::YCbCrBuffer b;
    b.mPlanes[0].mData = ycbcr[0].data;
    b.mPlanes[0].mStride = ycbcr[0].stride;
    b.mPlanes[0].mHeight = mTheoraInfo.frame_height;
    b.mPlanes[0].mWidth = mTheoraInfo.frame_width;
    b.mPlanes[0].mOffset = b.mPlanes[0].mSkip = 0;

    b.mPlanes[1].mData = ycbcr[1].data;
    b.mPlanes[1].mStride = ycbcr[1].stride;
    b.mPlanes[1].mHeight = mTheoraInfo.frame_height >> vdec;
    b.mPlanes[1].mWidth = mTheoraInfo.frame_width >> hdec;
    b.mPlanes[1].mOffset = b.mPlanes[1].mSkip = 0;

    b.mPlanes[2].mData = ycbcr[2].data;
    b.mPlanes[2].mStride = ycbcr[2].stride;
    b.mPlanes[2].mHeight = mTheoraInfo.frame_height >> vdec;
    b.mPlanes[2].mWidth = mTheoraInfo.frame_width >> hdec;
    b.mPlanes[2].mOffset = b.mPlanes[2].mSkip = 0;

    VideoInfo info;
    info.mDisplay = mInfo.mDisplay;
    RefPtr<VideoData> v = VideoData::Create(info,
                                              mImageContainer,
                                              aSample->mOffset,
                                              aSample->mTime,
                                              aSample->mDuration,
                                              b,
                                              aSample->mKeyframe,
                                              aSample->mTimecode,
                                              mInfo.mImage);

    if (!v) {
      LOG("Image allocation error source %ldx%ld display %ldx%ld picture %ldx%ld",
          mTheoraInfo.frame_width, mTheoraInfo.frame_height, mInfo.mDisplay.width, mInfo.mDisplay.height,
          mInfo.mImage.width, mInfo.mImage.height);
      return -1;
    }
    mCallback->Output(v);
    return 0;
  } else {
    LOG("Theora Decode error: %d", ret);
    return -1;
  }
}

void
TheoraDecoder::DecodeFrame(MediaRawData* aSample)
{
  if (DoDecodeFrame(aSample) == -1) {
    mCallback->Error();
  } else if (mTaskQueue->IsEmpty()) {
    mCallback->InputExhausted();
  }
}

nsresult
TheoraDecoder::Input(MediaRawData* aSample)
{
  nsCOMPtr<nsIRunnable> runnable(
    NS_NewRunnableMethodWithArg<RefPtr<MediaRawData>>(
      this, &TheoraDecoder::DecodeFrame,
      RefPtr<MediaRawData>(aSample)));
  mTaskQueue->Dispatch(runnable.forget());

  return NS_OK;
}

void
TheoraDecoder::DoDrain()
{
  mCallback->DrainComplete();
}

nsresult
TheoraDecoder::Drain()
{
  nsCOMPtr<nsIRunnable> runnable(
    NS_NewRunnableMethod(this, &TheoraDecoder::DoDrain));
  mTaskQueue->Dispatch(runnable.forget());

  return NS_OK;
}

/* static */
bool
TheoraDecoder::IsTheora(const nsACString& aMimeType)
{
  return aMimeType.EqualsLiteral("video/ogg; codecs=theora");
}

} // namespace mozilla
#undef LOG
