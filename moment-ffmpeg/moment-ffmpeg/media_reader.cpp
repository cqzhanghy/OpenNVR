/*  Moment Video Server - High performance media server
    Copyright (C) 2013 Dmitry Shatrov
    e-mail: shatrov@gmail.com

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <moment-ffmpeg/media_reader.h>
#include <moment-ffmpeg/time_checker.h>
#include <moment-ffmpeg/naming_scheme.h>
#include <moment-ffmpeg/ffmpeg_stream.h>
#include <string>

#ifdef PLATFORM_WIN32
#include "windows.h"
void usleep( int microseconds ) {
    Sleep( microseconds / 1000 );
}
#endif

using namespace M;
using namespace Moment;

namespace MomentFFmpeg {

static LogGroup libMary_logGroup_reader ("mod_ffmpeg.media_reader", LogLevel::E);
static LogGroup libMary_logGroup_frames ("mod_ffmpeg.media_reader_frames", LogLevel::E);

#define AV_RB32(x)									\
        (((Uint32)((const Byte*)(x))[0] << 24) |	\
        (((const Byte*)(x))[1] << 16) |				\
        (((const Byte*)(x))[2] <<  8) |				\
        ( (const Byte*)(x))[3])

#define AV_RB24(x)									\
        ((((const Byte*)(x))[0] << 16) |			\
        ( ((const Byte*)(x))[1] <<  8) |			\
        (  (const Byte*)(x))[2])

StateMutex FileReader::m_mutexFFmpeg;

static void RegisterFFMpeg(void)
{
    static Uint32 uiInitialized = 0;

    if(uiInitialized != 0)
        return;

    uiInitialized = 1;

    // global ffmpeg initialization
    av_register_all();
    avformat_network_init();
}

int FileReader::WriteB8ToBuffer(Int32 b, MemoryEx & memory)
{
    if(memory.size() >= memory.len())
        return -1;

    Size curPos = memory.size();
    Byte * pMem = memory.mem();
    pMem[curPos] = (Byte)b;

    memory.setSize(curPos + 1);

    if(memory.size() >= memory.len())
    {
        logE_ (_func, "Fail, size(", memory.size(), ") > memoryOut.len(", memory.len(), ")");
        return Result::Failure;
    }

    return Result::Success;
}

int FileReader::WriteB16ToBuffer(Uint32 val, MemoryEx & memory)
{
    if( WriteB8ToBuffer((Byte)(val >> 8 ), memory) == Result::Success &&
        WriteB8ToBuffer((Byte) val       , memory) == Result::Success)
    {
        return Result::Success;
    }

    return Result::Failure;
}

int FileReader::WriteB32ToBuffer(Uint32 val, MemoryEx & memory)
{
    if( WriteB8ToBuffer(       val >> 24 , memory) == Result::Success &&
        WriteB8ToBuffer((Byte)(val >> 16), memory) == Result::Success &&
        WriteB8ToBuffer((Byte)(val >> 8 ), memory) == Result::Success &&
        WriteB8ToBuffer((Byte) val       , memory) == Result::Success)
    {
        return Result::Success;
    }

    return Result::Failure;
}

int FileReader::WriteDataToBuffer(ConstMemory const memory, MemoryEx & memoryOut)
{
    Size inputSize = memory.len();

    if(memoryOut.size() + inputSize >= memoryOut.len())
    {
        logE_ (_func, "Fail, size(", memoryOut.size(), ") + inputSize(", inputSize, ") > memoryOut.len(", memoryOut.len(), ")");
        return Result::Failure;
    }

    if(inputSize >= memoryOut.len())
    {
        logE_ (_func, "inputSize(", inputSize, ") >= memoryOut.len(", memoryOut.len(), ")");
        return Result::Failure;
    }
    else if(inputSize > 0)
    {
        memcpy(memoryOut.mem() + memoryOut.size(), memory.mem(), inputSize);

        memoryOut.setSize(memoryOut.size() + inputSize);
    }

    return Result::Success;
}

static const Byte * AvcFindStartCodeInternal(const Byte *p, const Byte *end)
{
    const Byte *a = p + 4 - ((intptr_t)p & 3);

    for (end -= 3; p < a && p < end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }

    for (end -= 3; p < end; p += 4)
    {
        Uint32 x = *(const Uint32*)p;
        //      if ((x - 0x01000100) & (~x) & 0x80008000) // little endian
        //      if ((x - 0x00010001) & (~x) & 0x00800080) // big endian
        if ((x - 0x01010101) & (~x) & 0x80808080)
        {
            // generic
            if (p[1] == 0)
            {
                if (p[0] == 0 && p[2] == 1)
                    return p;
                if (p[2] == 0 && p[3] == 1)
                    return p+1;
            }

            if (p[3] == 0)
            {
                if (p[2] == 0 && p[4] == 1)
                    return p+2;
                if (p[4] == 0 && p[5] == 1)
                    return p+3;
            }
        }
    }

    for (end += 3; p < end; p++)
    {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }

    return end + 3;
}

static const Byte * AvcFindStartCode(const Byte *p, const Byte *end)
{
    const Byte * out = AvcFindStartCodeInternal(p, end);

    if(p < out && out < end && !out[-1])
        out--;

    return out;
}

int FileReader::AvcParseNalUnits(ConstMemory const mem, MemoryEx * pMemoryOut)
{
    const Byte *p = mem.mem();
    const Byte *end = p + mem.len();
    const Byte *nal_start, *nal_end;

    Int32 size = 0;

    nal_start = AvcFindStartCode(p, end);

    for (;;)
    {
        while (nal_start < end && !*(nal_start++));

        if (nal_start == end)
            break;

        nal_end = AvcFindStartCode(nal_start, end);

        if(pMemoryOut)
        {
            WriteB32ToBuffer(nal_end - nal_start, *pMemoryOut);
            WriteDataToBuffer(ConstMemory(nal_start, nal_end - nal_start), *pMemoryOut);
        }

        size += 4 + nal_end - nal_start;
        nal_start = nal_end;
    }

    return size;
}

int FileReader::IsomWriteAvcc(ConstMemory const memory, MemoryEx & memoryOut)
{
    if (memory.len() > 6)
    {
        Byte const * data = memory.mem();

        // check for h264 start code
        if (AV_RB32(data) == 0x00000001 || AV_RB24(data) == 0x000001)
        {
            MemorySafe allocMemory(memory.len() * 2);
            MemoryEx localMemory((Byte *)allocMemory.cstr(), allocMemory.len());

            if(AvcParseNalUnits(memory, &localMemory) < 0)
            {
                logE_ (_func, "AvcParseNalUnits fails");
                return Result::Failure;
            }

            Byte * buf = localMemory.mem();
            Byte * end = buf + localMemory.size();
            Uint32 sps_size = 0, pps_size = 0;
            Byte * sps = 0, *pps = 0;
            // look for sps and pps
            while (end - buf > 4)
            {
                Uint32 size;
                Byte nal_type;

                size = std::min((Uint32)AV_RB32(buf), (Uint32)(end - buf - 4));

                buf += 4;
                nal_type = buf[0] & 0x1f;

                if (nal_type == 7)
                {
                    // SPS
                    sps = buf;
                    sps_size = size;
                }
                else if (nal_type == 8)
                {
                    // PPS
                    pps = buf;
                    pps_size = size;
                }

                buf += size;
            }

            if (!sps || !pps || sps_size < 4 || sps_size > Uint16_Max || pps_size > Uint16_Max)
            {
                logE_ (_func, "Failed to get sps, pps");
                return Result::Failure;
            }

            WriteB8ToBuffer(1, memoryOut);			// version
            WriteB8ToBuffer(sps[1], memoryOut);	// profile
            WriteB8ToBuffer(sps[2], memoryOut);	// profile compat
            WriteB8ToBuffer(sps[3], memoryOut);	// level
            WriteB8ToBuffer(0xff, memoryOut);		// 6 bits reserved (111111) + 2 bits nal size length - 1 (11)
            WriteB8ToBuffer(0xe1, memoryOut);		// 3 bits reserved (111) + 5 bits number of sps (00001)

            WriteB16ToBuffer(sps_size, memoryOut);
            WriteDataToBuffer(ConstMemory(sps, sps_size), memoryOut);
            WriteB8ToBuffer(1, memoryOut);			// number of pps
            WriteB16ToBuffer(pps_size, memoryOut);
            WriteDataToBuffer(ConstMemory(pps, pps_size), memoryOut);
        }
        else
        {
            WriteDataToBuffer(memory, memoryOut);
        }
    }

    return Result::Success;
}

void FileReader::CloseCodecs(AVFormatContext * pAVFrmtCntxt)
{
    if(!pAVFrmtCntxt)
        return;

    for(unsigned int uiCnt = 0; uiCnt < pAVFrmtCntxt->nb_streams; ++uiCnt)
    {
        // close each AVCodec
        if(pAVFrmtCntxt->streams[uiCnt])
        {
            AVStream * pAVStream = pAVFrmtCntxt->streams[uiCnt];

            if(pAVStream->codec)
                avcodec_close(pAVStream->codec);
        }
    }
}

bool FileReader::IsInit()
{
    return (format_ctx != NULL);
}

bool FileReader::Init(StRef<String> & fileName, bool bFileDownload)
{
    DeInit();

    m_fileName = st_grab (new (std::nothrow) String (fileName->mem()));
    logD(reader, _func_, "m_fileName = ", m_fileName);

    const char * file_name_path = m_fileName->cstr();

    Result res = Result::Success;
    if(avformat_open_input(&format_ctx, file_name_path, NULL, NULL) == 0)
    {
        // Retrieve stream information
        m_mutexFFmpeg.lock();
        if(avformat_find_stream_info(format_ctx, NULL) >= 0)
        {
            // Dump information about file onto standard error
            av_dump_format(format_ctx, 0, file_name_path, 0);

            if(format_ctx->nb_streams <= 0)
            {
                logE_(_func_,"format_ctx->nb_streams <= 0");
                res = Result::Failure;
            }
        }
        else
        {
            logE_(_func_,"Fail to retrieve stream info");
            res = Result::Failure;
        }
        m_mutexFFmpeg.unlock();
    }
    else
    {
        logE_(_func_,"Fail to open file: ", m_fileName);
        res = Result::Failure;
    }

    if(res == Result::Success)
    {
        stream_params = new (std::nothrow) StreamParams[format_ctx->nb_streams];

        if(!stream_params)
        {
            logE_(_func_,"ffmpegStreamData::Init, Out of memory");
            res = Result::Failure;
        }
    }

    if(res == Result::Success)
    {
        int iActualStreams = 0;
        // Find the first video stream
        for(int i = 0; i < format_ctx->nb_streams; i++)
        {
            if(format_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
            {
                stream_params[i].video_params.codec_id = FFmpegStream::GetVideoCodecId(format_ctx->streams[i]->codec->codec_id);

                if(stream_params[i].video_params.codec_id != VideoStream::VideoCodecId::Unknown)
                {
                    stream_params[i].stream_type = VideoStream::Message::Type_Video;
                    ++iActualStreams;
                }
            }
            else if(format_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
            {
                stream_params[i].audio_params.codec_id = FFmpegStream::GetAudioCodecId(format_ctx->streams[i]->codec);

                if(stream_params[i].audio_params.codec_id != VideoStream::AudioCodecId::Unknown)
                {
                    stream_params[i].stream_type = VideoStream::Message::Type_Audio;
                    ++iActualStreams;
                }
            }
        }

        if(iActualStreams <= 0)
        {
            logE_(_func_,"ffmpegStreamData::Init, Didn't find any stream");
            res = Result::Failure;
        }
    }
    else
    {
        logE_(_func_,"ffmpegStreamData::Init, Didn't find a video stream");
    }

    if(res == Result::Success)
    {
        m_dDuration = (double)(format_ctx->duration) / AV_TIME_BASE;
        logD(reader, _func_,"m_dDuration: ", m_dDuration);
        res = (m_dDuration >= 0.0)? Result::Success: Result::Failure;
    }

    if(res == Result::Success)
    {
        if(!bFileDownload)
        {
            res = FileNameToUnixTimeStamp().Convert(m_fileName, m_initTimeOfRecord);
        }
        else
        {
            if (avio_open(&format_ctx->pb, m_fileName->cstr(), AVIO_FLAG_READ) < 0)
            {
                logE_(_func_, "fail to avio_open ", m_fileName);
                res = Result::Failure;
            }
            m_totalSize = avio_size(format_ctx->pb);
            avio_close(format_ctx->pb);

            res = Result::Success;
        }
    }

    if(res != Result::Success)
    {
        logE_(_func_, "init failed, run deinit");
        DeInit();
    }

    logD(reader, _func_, "init succeeded");

    return (res == Result::Success);
}

void FileReader::DeInit()
{
    if(stream_params)
    {
        // free memory
        delete [] stream_params;
        stream_params = NULL;
    }

    if(format_ctx)
    {
        CloseCodecs(format_ctx);

        avformat_close_input(&format_ctx);

        format_ctx = NULL;
    }

    m_fileName = NULL;

    m_initTimeOfRecord = 0;

    m_dDuration = -1.0;

    first_key_frame_received = false;

    m_totalSize = 0;
}

FileReader::FileReader()
{
    RegisterFFMpeg();

    format_ctx = NULL;

    m_initTimeOfRecord = 0;

    m_dDuration = -1.0;

    first_key_frame_received = false;

    stream_params = NULL;

    m_totalSize = 0;
}

FileReader::~FileReader()
{
    DeInit();
}

bool FileReader::ReadFrame(Frame & readframe)
{
    TimeChecker tc;tc.Start();

    if(!IsInit())
    {
        logE_(_func_, "IsInit returns false");
        return false;
    }

    AVPacket packet = {};

    while(true)
    {
        AVPacket readpacket = {};
        int err = 0;

        if((err = av_read_frame(format_ctx, &readpacket)) < 0)
        {
            if((size_t)err == AVERROR(EAGAIN))
            {
                 logD (frames, _func, "av_read_frame, err == AVERROR(EAGAIN), err = ", err);
                 usleep(100000);
                 continue;
            }
            else
            {
                char errmsg[4096] = {};
                av_strerror (err, errmsg, 4096);
                logD (frames, "Error while av_read_frame: ", errmsg);
                return false;
            }
        }

        // let it be just video for now
        if(stream_params[readpacket.stream_index].stream_type != VideoStream::Message::Type_None)
        {
            bool bValidPacket = true;

            if(!first_key_frame_received)
            {
                if((stream_params[readpacket.stream_index].stream_type == VideoStream::Message::Type_Video) && (readpacket.flags & AV_PKT_FLAG_KEY))
                {
                    first_key_frame_received = true;
                    logD (frames, _func, "first_key_frame_received");
                }
                else
                {
                    // check if we have any video stream, it is crazy case (if we do not have video), but JIC.
                    bool bHasVideoStream = false;

                    for(int i = 0; i < (format_ctx->nb_streams) && !bHasVideoStream; i++)
                    {
                        bHasVideoStream = (stream_params[readpacket.stream_index].stream_type == VideoStream::Message::Type_Video);
                    }

                    if(bHasVideoStream)
                    {
                        logD (frames, _func, "SKIPPED PACKET");
                        // we skip first frames until we get key frame to avoid corrupted playing
                        bValidPacket = false;
                    }
                    else
                    {
                        first_key_frame_received = true;
                        logD (frames, _func, "first_key_frame_received AND IT IS AUDIO!!! WARNING!!! SOS!!!");
                    }
                }
            }

            if(bValidPacket)
            {
                packet = readpacket;

                break;
            }
        }

        logD (frames, _func, "SKIPPED PACKET for this stream = ", readpacket.stream_index);
        av_free_packet(&readpacket);
    }   // while(1)

    StreamParams * pStreamParams = &stream_params[packet.stream_index];
    AVCodecContext * pctx = format_ctx->streams[packet.stream_index]->codec;

    if(pctx->extradata && pctx->extradata_size > 0)
    {
        // set header info
        MemorySafe & codec_data = pStreamParams->codec_data;
        MemorySafe & extra_data = pStreamParams->extra_data;
        bool bEmptyHeader = true;

        if(extra_data.len() != pctx->extradata_size || memcmp(pctx->extradata, extra_data.cstr(), extra_data.len()))
        {
            if(pStreamParams->stream_type == VideoStream::Message::Type_Video &&
               pStreamParams->video_params.codec_id == VideoStream::VideoCodecId::AVC)
            {
                MemorySafe allocMemory(pctx->extradata_size * 2);
                MemoryEx memEx((Byte *)allocMemory.cstr(), allocMemory.len());
                memset(memEx.mem(), 0, memEx.len());

                IsomWriteAvcc(ConstMemory(pctx->extradata, pctx->extradata_size), memEx);

                if(memEx.len())
                {
                    bEmptyHeader = false;
                    logD(frames, _func, "AVC Codec data has changed");
                    codec_data.allocate(memEx.len());

                    memcpy(codec_data.cstr(), memEx.mem(), memEx.len());
                    codec_data.setLength(memEx.len());
                    readframe.video_info.header_type = VideoStream::VideoFrameType::AvcSequenceHeader;
                }
            }
            else if(pStreamParams->stream_type == VideoStream::Message::Type_Audio &&
                    (pStreamParams->audio_params.codec_id == VideoStream::AudioCodecId::AAC ||
                     pStreamParams->audio_params.codec_id == VideoStream::AudioCodecId::Speex))
            {
                // in this case extra_data == codec_data
                codec_data.allocate(pctx->extradata_size);

                memcpy(codec_data.cstr(), pctx->extradata, pctx->extradata_size);
                codec_data.setLength(pctx->extradata_size);
                bEmptyHeader = false;
                readframe.audio_info.header_type = (pStreamParams->audio_params.codec_id == VideoStream::AudioCodecId::AAC) ?
                        VideoStream::AudioFrameType::AacSequenceHeader : VideoStream::AudioFrameType::SpeexHeader;
                logD(frames, _func, "AAC or Speex Codec data has changed");
            }
        }

        if(!bEmptyHeader)
        {
            readframe.header = ConstMemory(codec_data.cstr(), codec_data.len());

            // set extra_data
            extra_data.allocate(pctx->extradata_size);
            memcpy(extra_data.cstr(), pctx->extradata, pctx->extradata_size);
            extra_data.setLength(pctx->extradata_size);
        }
    }

    // retrive frame
    readframe.frame = ConstMemory(packet.data, packet.size);
    readframe.frame_type = pStreamParams->stream_type;
    readframe.timestamp_nanosec = m_initTimeOfRecord + packet.pts / (double)format_ctx->streams[packet.stream_index]->time_base.den * 1000000000LL;
    readframe.src_packet = packet;

    if(pStreamParams->stream_type == VideoStream::Message::Type_Video)
    {
        readframe.video_info.codec_id = pStreamParams->video_params.codec_id;
        readframe.video_info.type = (packet.flags & AV_PKT_FLAG_KEY) ?
                    VideoStream::VideoFrameType::KeyFrame : VideoStream::VideoFrameType::InterFrame;
    }
    else        // audio
    {
        readframe.audio_info.codec_id = pStreamParams->audio_params.codec_id;
        readframe.audio_info.type = VideoStream::AudioFrameType::RawData;
    }

    logD(frames, _func_, "NEW FRAME GOT");
    logD(frames, _func_, "readframe.header.len() = [", readframe.header.len(), "]");
    logD(frames, _func_, "readframe.frame.len() = [", readframe.frame.len(), "]");
    logD(frames, _func_, "readframe.timestamp_nanosec = [", readframe.timestamp_nanosec, "]");
    logD(frames, _func_, "readframe.bKeyFrame = [", (readframe.video_info.type == VideoStream::VideoFrameType::KeyFrame), "]");
    logD(frames, _func_, "readframe.frameType = [", (int)readframe.frame_type, "]");

    Time t;tc.Stop(&t);
    logD(frames, _func_, "FileReader.ReadFrame exectime = [", t, "]");

    return true;
}

void FileReader::FreeFrame(Frame & readframe)
{
    av_free_packet(&readframe.src_packet);
    memset(&readframe.src_packet, 0, sizeof(readframe.src_packet));
}

bool FileReader::Seek(double dSeconds)
{
    TimeChecker tc;tc.Start();

    if(!IsInit())
    {
        logD(reader, _func_, "IsInit returns false");
        return false;
    }

    int64_t llTimeStamp = (int64_t)(dSeconds * AV_TIME_BASE);
    int res = avformat_seek_file(format_ctx, -1, Int64_Min, llTimeStamp, Int64_Max, 0);

    // alter variant: avformat_seek_file(ic, -1, INT64_MIN, llTimeStamp, llTimeStamp, 0);

    Time t;tc.Stop(&t);
    logD(reader, _func_, "FileReader.Seek exectime = [", t, "]");

    return (res >= 0);
}

// end of FileReader implementation

mt_mutex (mutex) bool
MediaReader::tryOpenNextFile ()
{
    TimeChecker tc;tc.Start();

    if (m_itr == m_channelFileDiskTimes.end())
    {
        logD(reader, _func_, "end of files");
        return false;
    }

    StRef<String> cur_filename = st_makeString(m_itr->second.diskName.c_str(), "/", m_itr->first.c_str(), ".flv");
    logD (reader, _func, "new filename: ", cur_filename);

    m_itr++;

    if(!m_fileReader.Init(cur_filename))
    {
        logE_(_func_,"m_fileReader.Init failed");
        return false;
    }

    Time unix_time_stamp = 0;

    if(FileNameToUnixTimeStamp().Convert(cur_filename, unix_time_stamp) == Result::Success)
    {
        unix_time_stamp /= 1000000000LL;
        if(unix_time_stamp < start_unixtime_sec)
        {
            logD (reader, _func,"unix_time_stamp(", unix_time_stamp, ") < start_unixtime_sec (", start_unixtime_sec, ")");
            double dNumSeconds = (start_unixtime_sec - unix_time_stamp);
            logD (reader, _func,"we will skip ", dNumSeconds, "seconds");
            // DISCUSSION: may be we must get duration from m_fileReader and if it less than dNumSeconds
            // call tryOpenNextFile again (recursively).

            if(!m_fileReader.Seek(dNumSeconds))
            {
                return false;
            }
        }
    }
    else
    {
        return false;
    }

    Time t;tc.Stop(&t);
    logD(reader, _func_, "MediaReader.tryOpenNextFile exectime = [", t, "]");

    return true;
}

mt_mutex (mutex) MediaReader::ReadFrameResult
MediaReader::sendFrame (const FileReader::Frame & inputFrame,
                        ReadFrameBackend const * const read_frame_cb,
                        void                   * const read_frame_cb_data)
{
    TimeChecker tc;tc.Start();

    ReadFrameResult client_res = ReadFrameResult_Success;

    if (inputFrame.header.len())
    {
        Size msg_len = 0;

        PagePool::PageListHead page_list;

        page_pool->getFillPages (&page_list,
                                 inputFrame.header);
        msg_len += inputFrame.header.len();

        if(inputFrame.frame_type == VideoStream::Message::Type_Video)
        {
            VideoStream::VideoMessage msg;
            msg.timestamp_nanosec = inputFrame.timestamp_nanosec;
            msg.prechunk_size = 0;
            msg.frame_type = inputFrame.video_info.header_type;
            msg.codec_id = inputFrame.video_info.codec_id;

            msg.page_pool = page_pool;
            msg.page_list = page_list;
            msg.msg_len = msg_len;
            msg.msg_offset = 0;
            msg.is_saved_frame = false;

            client_res = read_frame_cb->videoFrame (&msg, read_frame_cb_data);
        }
        else if(inputFrame.frame_type == VideoStream::Message::Type_Audio)
        {
            VideoStream::AudioMessage msg;
            msg.timestamp_nanosec = inputFrame.timestamp_nanosec;
            msg.prechunk_size = 0;
            msg.frame_type = inputFrame.audio_info.header_type;
            msg.codec_id = inputFrame.audio_info.codec_id;

            msg.page_pool = page_pool;
            msg.page_list = page_list;
            msg.msg_len = msg_len;
            msg.msg_offset = 0;

            client_res = read_frame_cb->audioFrame (&msg, read_frame_cb_data);
        }
        else
        {
            logE_(_func_, "unknown frame type");
        }

        page_pool->msgUnref (page_list.first);
    }

    if(inputFrame.frame_type == VideoStream::Message::Type_Video)
    {
        VideoStream::VideoMessage msg;
        msg.frame_type = inputFrame.video_info.type;
        msg.codec_id = inputFrame.video_info.codec_id;

        PagePool::PageListHead page_list;

        page_pool->getFillPages (&page_list,
                                 inputFrame.frame);

        Size msg_len = inputFrame.frame.len();
        msg.timestamp_nanosec = inputFrame.timestamp_nanosec;

        msg.prechunk_size = 0;

        msg.page_pool = page_pool;
        msg.page_list = page_list;
        msg.msg_len = msg_len;
        msg.msg_offset = 0;
        msg.is_saved_frame = false;

        client_res = read_frame_cb->videoFrame (&msg, read_frame_cb_data);

        page_pool->msgUnref (page_list.first);
    }
    else if(inputFrame.frame_type == VideoStream::Message::Type_Audio)
    {
        VideoStream::AudioMessage msg;
        msg.frame_type = inputFrame.audio_info.type;
        msg.codec_id = inputFrame.audio_info.codec_id;

        PagePool::PageListHead page_list;

        page_pool->getFillPages (&page_list,
                                 inputFrame.frame);

        Size msg_len = inputFrame.frame.len();
        msg.timestamp_nanosec = inputFrame.timestamp_nanosec;

        msg.prechunk_size = 0;

        msg.page_pool = page_pool;
        msg.page_list = page_list;
        msg.msg_len = msg_len;
        msg.msg_offset = 0;

        client_res = read_frame_cb->audioFrame (&msg, read_frame_cb_data);

        page_pool->msgUnref (page_list.first);
    }
    else
    {
        logE_(_func_, "unknown frame type");
    }

    Time t;tc.Stop(&t);
    logD(reader, _func_, "MediaReader.sendFrame exectime = [", t, "]");

    if (client_res != ReadFrameResult_Success)
    {
        logD (reader, _func, "return burst");
        return client_res;
    }

    return ReadFrameResult_Success;
}
static int64_t sizecounter = 0;
MediaReader::ReadFrameResult
MediaReader::readMoreData (ReadFrameBackend const * const read_frame_cb,
                           void                   * const read_frame_cb_data)
{
    logD(frames, _func_);

    ReadFrameResult rf_res = ReadFrameResult_Success;

    logD(frames, _func_, "bDownload is : ", bDownload);
    if(bDownload)
    {
        logD(frames, _func_, "Just download mp4 file: ", fileDownload);
        if (!m_fileReader.IsInit())
        {
            if(!m_fileReader.Init(fileDownload, true))
            {
                logE_(_func_,"m_fileReader.Init failed");
                return ReadFrameResult_NoData;
            }
        }

        // read packets from file
        while (1)
        {
            Result res = Result::Failure;
            FileReader::Frame frame;

            if(m_fileReader.ReadFrame(frame))
            {
                sizecounter += frame.GetPacket().size;
                rf_res = sendFrame (frame, read_frame_cb, read_frame_cb_data);
                if (rf_res == ReadFrameResult_BurstLimit) {
                    logD (frames, _func, "session 0x", fmt_hex, (UintPtr) this, ": BurstLimit, ", "Filename: ", m_fileReader.GetFilename());
                    break;
                } else
                if (rf_res == ReadFrameResult_Finish) {
                    break;
                } else
                if (rf_res == ReadFrameResult_Success) {
                    res = Result::Success;
                } else {
                    res = Result::Failure;
                }

                m_fileReader.FreeFrame(frame);
            }
            else
            {
                logD(frames, _func_, "sizecounter = ", sizecounter);
                rf_res = ReadFrameResult_NoData;
                break;
            }
        }

        return rf_res;
    }
    else
    {
    TimeChecker tc;tc.Start();

    ReadFrameResult rf_res = ReadFrameResult_Success;

    if (!m_fileReader.IsInit())
    {
        logD(frames, _func_, "m_fileReader isnt init");
        if (!tryOpenNextFile ())
        {
            logD(frames, _func_, "tryOpenNextFile returned false");
            return ReadFrameResult_NoData;
        }
    }

    // read packets from file
    while (1)
    {
        Result res = Result::Failure;
        FileReader::Frame frame;

        if(m_fileReader.ReadFrame(frame))
        {
            rf_res = sendFrame (frame, read_frame_cb, read_frame_cb_data);
            if (rf_res == ReadFrameResult_BurstLimit) {
                logD (frames, _func, "session 0x", fmt_hex, (UintPtr) this, ": BurstLimit, ", "Filename: ", m_fileReader.GetFilename());
                break;
            } else
            if (rf_res == ReadFrameResult_Finish) {
                break;
            } else
            if (rf_res == ReadFrameResult_Success) {
                res = Result::Success;
            } else {
                res = Result::Failure;
            }

            m_fileReader.FreeFrame(frame);
        }
        else
        {
            if (!tryOpenNextFile ())
            {
                rf_res = ReadFrameResult_NoData;
                break;
            }
        }
    }

    Time t;tc.Stop(&t);
    logD (frames, _func_, "MediaReader.readMoreData exectime = [", t, "]");

    return rf_res;
    }
}

mt_const void
MediaReader::init (PagePool    * const mt_nonnull page_pool,
                   ChannelChecker::ChannelFileDiskTimes & channelFileDiskTimes,
                   ConstMemory   const stream_name,
                   Time          const start_unixtime_sec,
                   Size          const burst_size_limit,
                   StRef<String> const fileDownload,
                   bool                bDownload)
{
    this->page_pool = page_pool;
    this->m_channelFileDiskTimes = channelFileDiskTimes;
    this->start_unixtime_sec = start_unixtime_sec;
    this->burst_size_limit = burst_size_limit;

    this->fileDownload = fileDownload;
    this->bDownload = bDownload;

    logD (reader, _func_, "start_unixtime_sec = ", this->start_unixtime_sec);
    logD (reader, _func_, "burst_size_limit = ", this->burst_size_limit);

    logD (reader, _func, "stream_name: ", (const char *)stream_name.mem());

    bool bFileIsFound = false;
    for(m_itr = m_channelFileDiskTimes.begin(); m_itr != m_channelFileDiskTimes.end(); m_itr++)
    {
        if(m_itr->second.times.timeStart <= start_unixtime_sec && m_itr->second.times.timeEnd > start_unixtime_sec)
        {
            bFileIsFound = true;
            break; // file is found
        }
    }
    if(!bFileIsFound)
    {
        logE(reader, _func_, "there is no files with such timestamps in storage");
        return;
    }
}

mt_mutex (mutex) void
MediaReader::releaseSequenceHeaders_unlocked ()
{
    if (got_aac_seq_hdr) {
        page_pool->msgUnref (aac_seq_hdr.first);
        got_aac_seq_hdr = false;
    }

    if (got_avc_seq_hdr) {
        page_pool->msgUnref (avc_seq_hdr.first);
        got_avc_seq_hdr = false;
    }
}

MediaReader::~MediaReader ()
{
    mutex.lock ();
    releaseSequenceHeaders_unlocked ();
    mutex.unlock ();

}

}

