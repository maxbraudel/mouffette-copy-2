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

# MP4 indexes use DTS. A keyframe selected by DTS can have a PTS later than the
# desired image when B frames reorder timestamps. Walk back to a decodable GOP
# whose keyframe PTS is at or before the target; keep the presentation clock at
# the requested position. Only a few compressed packets are inspected.
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
                    err = av_seek_frame(m_context, index, dts == AV_NOPTS_VALUE ? cursor : dts, AVSEEK_FLAG_BACKWARD);
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
mouffette_patch_ffmpeg(playbackengine/qffmpegstreamdecoder.cpp
    [=[        else
            decodeMedia(packet);
    };]=]
    [=[        else
            decodeMedia(packet);
        if (!packet.isValid() && m_sessionCtx.pendingVideoFrame.isValid())
            onFrameFound(std::exchange(m_sessionCtx.pendingVideoFrame, Frame{}));
    };]=])
mouffette_patch_ffmpeg(playbackengine/qffmpegstreamdecoder.cpp
    "frame.isValid() && frame.absoluteEnd() < m_sessionCtx.absSeekPos"
    "frame.isValid() && (m_trackType == QPlatformMediaPlayer::VideoStream ? frame.absoluteEnd() <= m_sessionCtx.absSeekPos : frame.absoluteEnd() < m_sessionCtx.absSeekPos)")
mouffette_patch_ffmpeg(playbackengine/qffmpegrenderer.cpp
    "frame.isValid() && frame.absoluteEnd() < seekPosition()"
    "frame.isValid() && (frame.codecContext() && frame.codecContext()->context()->codec_type == AVMEDIA_TYPE_VIDEO ? frame.absoluteEnd() <= seekPosition() : frame.absoluteEnd() < seekPosition())")
