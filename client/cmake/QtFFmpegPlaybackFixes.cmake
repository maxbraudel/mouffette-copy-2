# Pinned Qt 6.11.2 fixes exercised by ResidentMedia's delayed-frame/VFR seeks.
function(mouffette_patch_ffmpeg relative old_text new_text)
    set(_file "${_ffmpeg}/${relative}")
    file(READ "${_file}" _text)
    string(FIND "${_text}" "${new_text}" _found)
    if(NOT _found EQUAL -1)
        return()
    endif()
    string(FIND "${_text}" "${old_text}" _found)
    if(_found EQUAL -1)
        message(FATAL_ERROR "Review the Qt FFmpeg playback patch for ${relative}")
    endif()
    string(REPLACE "${old_text}" "${new_text}" _text "${_text}")
    file(WRITE "${_file}" "${_text}")
endfunction()

# FFmpeg needs the packet timebase to adjust audio PTS/duration when trimming
# encoder priming or trailing samples. Without it, partially trimmed AAC frames
# keep the old timestamp and emit "Could not update timestamps for skipped samples".
mouffette_patch_ffmpeg(playbackengine/qffmpegcodeccontext.cpp
    "ret = avcodec_open2(context.get(), decoder->get(), opts);"
    "context->pkt_timebase = stream->time_base;\n    ret = avcodec_open2(context.get(), decoder->get(), opts);")

# Migrate previously patched build trees before matching the complete patch.
# Otherwise its original av_seek_frame line would receive a second nested patch.
set(_previous_seek_restore "err = av_seek_frame(m_context, index, dts == AV_NOPTS_VALUE ? cursor : dts, AVSEEK_FLAG_BACKWARD);")
file(READ "${_ffmpeg}/playbackengine/qffmpegdemuxer.cpp" _demuxer_seek_source)
string(FIND "${_demuxer_seek_source}" "${_previous_seek_restore}" _previous_seek_restore_offset)
if(NOT _previous_seek_restore_offset EQUAL -1)
    string(REPLACE "${_previous_seek_restore}"
        "err = av_seek_frame(m_context, index, cursor, AVSEEK_FLAG_BACKWARD);"
        _demuxer_seek_source "${_demuxer_seek_source}")
    file(WRITE "${_ffmpeg}/playbackengine/qffmpegdemuxer.cpp" "${_demuxer_seek_source}")
endif()

# MP4 indexes use DTS. A selected keyframe can have a PTS later than the
# desired image when B frames reorder timestamps. Walk back to a decodable GOP
# whose keyframe PTS is at or before the target; keep the presentation clock at
# the requested position. Only a few compressed packets are inspected.
# Restore the exact seek cursor used to inspect the accepted packet. MOV seeks
# interpret timestamps as PTS; restoring a packet's DTS can jump back another GOP.
mouffette_patch_ffmpeg(playbackengine/qffmpegdemuxer.cpp
    "auto err = av_seek_frame(m_context, -1, seekPos.get(), AVSEEK_FLAG_BACKWARD);"
    [=[auto err = av_seek_frame(m_context, -1, seekPos.get(), AVSEEK_FLAG_BACKWARD);
        for (const auto &[index, streamData] : m_streams) {
            Q_UNUSED(streamData);
            AVStream *stream = m_context->streams[index];
            if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
            const int64_t target = av_rescale_q(seekPos.get(), AVRational{1, AV_TIME_BASE}, stream->time_base);
            int64_t cursor = target;
            AVPacketUPtr packet{ av_packet_alloc() };
            if (!packet) break;
            while ((err = av_seek_frame(m_context, index, cursor, AVSEEK_FLAG_BACKWARD)) >= 0) {
                int readResult;
                do {
                    av_packet_unref(packet.get());
                    readResult = av_read_frame(m_context, packet.get());
                } while (readResult >= 0 && packet->stream_index != index);
                if (readResult < 0) {
                    err = av_seek_frame(m_context, index, cursor, AVSEEK_FLAG_BACKWARD);
                    break;
                }
                const int64_t dts = packet->dts;
                const int64_t pts = packet->pts;
                if (pts == AV_NOPTS_VALUE || pts <= target || dts == AV_NOPTS_VALUE || dts >= cursor) {
                    err = av_seek_frame(m_context, index, cursor, AVSEEK_FLAG_BACKWARD);
                    break;
                }
                cursor = dts - 1;
            }
            break;
        }]=])

# One-frame lookahead gives the actual presentation interval, including VFR
# holds and gaps. It adds one frame, never a duration-sized decoded cache.
mouffette_patch_ffmpeg(playbackengine/qffmpegframe_p.h
    "TrackDuration duration() const { return data().duration; }"
    "TrackDuration duration() const { return data().duration; }\n    void setDuration(TrackDuration duration) { data().duration = duration; }")
mouffette_patch_ffmpeg(playbackengine/qffmpegstreamdecoder_p.h
    "QQueue<Packet> packets = {};"
    "QQueue<Packet> packets = {};\n        Frame pendingVideoFrame;")
mouffette_patch_ffmpeg(playbackengine/qffmpegstreamdecoder.cpp
    "onFrameFound({ m_sessionCtx.offset, std::move(avFrame), m_codecContext, id() });"
    [=[Frame next(m_sessionCtx.offset, std::move(avFrame), m_codecContext, id());
        if (m_trackType == QPlatformMediaPlayer::VideoStream) {
            Frame previous = std::exchange(m_sessionCtx.pendingVideoFrame, std::move(next));
            if (previous.isValid()) {
                const auto duration = m_sessionCtx.pendingVideoFrame.absolutePts() - previous.absolutePts();
                if (duration > TrackDuration(0)) previous.setDuration(duration);
                onFrameFound(previous);
            }
        } else {
            onFrameFound(next);
        }]=])
# At EOF the final video image remains visible through a longer audio track.
# Extend its interval before seek filtering without moving either track's PTS.
mouffette_patch_ffmpeg(playbackengine/qffmpegcodeccontext_p.h
    "uint streamIndex() const { return d->stream->index; }"
    [=[uint streamIndex() const { return d->stream->index; }
    TrackPosition mediaEnd() const {
        return TrackPosition(d->formatContext->duration == AV_NOPTS_VALUE
                                 ? 0 : d->formatContext->duration);
    }]=])
set(_old_video_drain [=[        if (!packet.isValid() && m_sessionCtx.pendingVideoFrame.isValid())
            onFrameFound(std::exchange(m_sessionCtx.pendingVideoFrame, Frame{}));]=])
set(_video_drain [=[        if (!packet.isValid() && m_sessionCtx.pendingVideoFrame.isValid()) {
            Frame last = std::exchange(m_sessionCtx.pendingVideoFrame, Frame{});
            const auto heldDuration = m_codecContext.mediaEnd() - last.startTime();
            if (heldDuration > last.duration()) last.setDuration(heldDuration);
            onFrameFound(last);
        }]=])
# Upgrade build trees that already contain the previous lookahead patch.
file(READ "${_ffmpeg}/playbackengine/qffmpegstreamdecoder.cpp" _decoder_source)
string(FIND "${_decoder_source}" "${_old_video_drain}" _old_video_drain_offset)
if(NOT _old_video_drain_offset EQUAL -1)
    string(REPLACE "${_old_video_drain}" "${_video_drain}" _decoder_source "${_decoder_source}")
    file(WRITE "${_ffmpeg}/playbackengine/qffmpegstreamdecoder.cpp" "${_decoder_source}")
endif()
mouffette_patch_ffmpeg(playbackengine/qffmpegstreamdecoder.cpp
    [=[        else
            decodeMedia(packet);
    };]=]
    "        else\n            decodeMedia(packet);\n${_video_drain}\n    };")
mouffette_patch_ffmpeg(playbackengine/qffmpegstreamdecoder.cpp
    "frame.isValid() && frame.absoluteEnd() < m_sessionCtx.absSeekPos"
    "frame.isValid() && (m_trackType == QPlatformMediaPlayer::VideoStream ? frame.absoluteEnd() <= m_sessionCtx.absSeekPos : frame.absoluteEnd() < m_sessionCtx.absSeekPos)")
mouffette_patch_ffmpeg(playbackengine/qffmpegrenderer.cpp
    "frame.isValid() && frame.absoluteEnd() < seekPosition()"
    "frame.isValid() && (frame.codecContext() && frame.codecContext()->context()->codec_type == AVMEDIA_TYPE_VIDEO ? frame.absoluteEnd() <= seekPosition() : frame.absoluteEnd() < seekPosition())")
