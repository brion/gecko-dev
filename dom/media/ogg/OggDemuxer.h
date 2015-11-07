/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#if !defined(OggDemuxer_h_)
#define OggDemuxer_h_

#include "nsTArray.h"
#include "MediaDataDemuxer.h"
#include "OggCodecState.h"
#include "OggCodecStore.h"

namespace mozilla {

class NesteggPacketHolder;

class OggTrackDemuxer;

class OggDemuxer : public MediaDataDemuxer
{
public:
  explicit OggDemuxer(MediaResource* aResource);

  RefPtr<InitPromise> Init() override;

  bool HasTrackType(TrackInfo::TrackType aType) const override;

  uint32_t GetNumberTracks(TrackInfo::TrackType aType) const override;

  UniquePtr<TrackInfo> GetTrackInfo(TrackInfo::TrackType aType, size_t aTrackNumber) const;

  already_AddRefed<MediaTrackDemuxer> GetTrackDemuxer(TrackInfo::TrackType aType,
                                                      uint32_t aTrackNumber) override;

  bool IsSeekable() const override;

  UniquePtr<EncryptionInfo> GetCrypto() override;

  media::TimeIntervals GetBuffered();

  nsresult SeekInternal(const media::TimeUnit& aTarget);

  bool GetOffsetForTime(uint64_t aTime, int64_t* aOffset);

  bool isTheoraKeyframe(ogg_packet* pkt);

  // Demux next Ogg packet
  RefPtr<MediaRawData> GetNextPacket(TrackInfo::TrackType aType);

  nsresult Reset();

  // for OggTrackDemuxer::Reset
  nsresult ResetTrackState(TrackInfo::TrackType aType);

private:

  static const nsString GetKind(const nsCString& aRole);
  static void InitTrack(MessageField* aMsgInfo,
                      TrackInfo* aInfo,
                      bool aEnable);

  ~OggDemuxer();
  void Cleanup();

  // Read enough of the file to identify track information and header
  // packets necessary for decoding to begin.
  nsresult ReadMetadata();

  // Read a page of data from the Ogg file. Returns true if a page has been
  // read, false if the page read failed or end of file reached.
  bool ReadOggPage(ogg_page* aPage);

	// Send a page off to the individual streams it belongs to.
	// Reconstructed packets, if any are ready, will be available
	// on the individual OggCodecStates.
  void DemuxOggPage(ogg_page* aPage);

	// Read data and demux until a packet is available on the given stream state
	ogg_packet *DemuxUntilPacketAvailable(OggCodecState *state);

  // Reads and decodes header packets for aState, until either header decode
  // fails, or is complete. Initializes the codec state before returning.
  // Returns true if reading headers and initializtion of the stream
  // succeeds.
  bool ReadHeaders(OggCodecState* aState, MediaByteBuffer* aCodecSpecificConfig);

  // Reads the next link in the chain.
  bool ReadOggChain();

  // Set this media as being a chain and notifies the state machine that the
  // media is no longer seekable.
  void SetChained(bool aIsChained);

  // Returns the next Ogg packet for an bitstream/codec state. Returns a
  // pointer to an ogg_packet on success, or nullptr if the read failed.
  // The caller is responsible for deleting the packet and its |packet| field.
  //ogg_packet* NextOggPacket(OggCodecState* aCodecState);

  // Fills aTracks with the serial numbers of each active stream, for use by
  // various SkeletonState functions.
  void BuildSerialList(nsTArray<uint32_t>& aTracks);

  // Setup target bitstreams for decoding.
  bool SetupTargetTheora();
  bool SetupTargetVorbis();
  bool SetupTargetOpus();
  void SetupTargetSkeleton();
  void SetupMediaTracksInfo(const nsTArray<uint32_t>& aSerials);


	ogg_uint32_t GetPageChecksum(ogg_page* page);

  // Get the end time of aEndOffset. This is the playback position we'd reach
  // after playback finished at aEndOffset.
  int64_t RangeEndTime(int64_t aEndOffset);

  // Get the end time of aEndOffset, without reading before aStartOffset.
  // This is the playback position we'd reach after playback finished at
  // aEndOffset. If bool aCachedDataOnly is true, then we'll only read
  // from data which is cached in the media cached, otherwise we'll do
  // regular blocking reads from the media stream. If bool aCachedDataOnly
  // is true, this can safely be called on the main thread, otherwise it
  // must be called on the state machine thread.
  int64_t RangeEndTime(int64_t aStartOffset,
                       int64_t aEndOffset,
                       bool aCachedDataOnly);

  // Get the start time of the range beginning at aOffset. This is the start
  // time of the first frame and or audio sample we'd be able to play if we
  // started playback at aOffset.
  int64_t RangeStartTime(int64_t aOffset);


  MediaInfo mInfo;
  nsTArray<RefPtr<OggTrackDemuxer>> mDemuxers;

  OggCodecStore mCodecStore;

  // Decode state of the Theora bitstream we're decoding, if we have video.
  TheoraState* mTheoraState;

  // Decode state of the Vorbis bitstream we're decoding, if we have audio.
  VorbisState* mVorbisState;

  // Decode state of the Opus bitstream we're decoding, if we have one.
  OpusState *mOpusState;

  // Get the bitstream decode state for the given track type
  OggCodecState *GetTrackCodecState(TrackInfo::TrackType aType) const;

  // Represents the user pref media.opus.enabled at the time our
  // contructor was called. We can't check it dynamically because
  // we're not on the main thread;
  bool mOpusEnabled;

  // Decode state of the Skeleton bitstream.
  SkeletonState* mSkeletonState;

  // Ogg decoding state.
  ogg_sync_state mOggState;

  // Vorbis/Opus/Theora data used to compute timestamps. This is written on the
  // decoder thread and read on the main thread. All reading on the main
  // thread must be done after metadataloaded. We can't use the existing
  // data in the codec states due to threading issues. You must check the
  // associated mTheoraState or mVorbisState pointer is non-null before
  // using this codec data.
  uint32_t mVorbisSerial;
  uint32_t mOpusSerial;
  uint32_t mTheoraSerial;
  vorbis_info mVorbisInfo;
  int mOpusPreSkip;
  th_info mTheoraInfo;

  // Booleans to indicate if we have audio and/or video data
  bool mHasVideo;
  bool mHasAudio;

  // The picture region inside Theora frame to be displayed, if we have
  // a Theora video track.
  nsIntRect mPicture;

  // True if we are decoding a chained ogg. Reading or writing to this member
  // should be done with |mMonitor| acquired.
  bool mIsChained;

  // Number of audio frames decoded so far.
  int64_t mDecodedAudioFrames;

  MediaResourceIndex mResource;
};

class OggTrackDemuxer : public MediaTrackDemuxer
{
public:
  OggTrackDemuxer(OggDemuxer* aParent,
                  TrackInfo::TrackType aType,
                  uint32_t aTrackNumber);

  UniquePtr<TrackInfo> GetInfo() const override;

  RefPtr<SeekPromise> Seek(media::TimeUnit aTime) override;

  RefPtr<SamplesPromise> GetSamples(int32_t aNumSamples = 1) override;

  void Reset() override;

  RefPtr<SkipAccessPointPromise> SkipToNextRandomAccessPoint(media::TimeUnit aTimeThreshold) override;

  media::TimeIntervals GetBuffered() override;

  void BreakCycles() override;

private:
  friend class OggDemuxer;
  ~OggTrackDemuxer();
  void SetNextKeyFrameTime();
  RefPtr<MediaRawData> NextSample ();
  RefPtr<OggDemuxer> mParent;
  TrackInfo::TrackType mType;
  UniquePtr<TrackInfo> mInfo;

  // Queued sample extracted by the demuxer, but not yet returned.
  RefPtr<MediaRawData> mQueuedSample;
};

} // namespace mozilla

#endif
