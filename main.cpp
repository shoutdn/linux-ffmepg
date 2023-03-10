/*
 * Copyright (c) 2012 Stefano Sabatini
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * Demuxing and decoding example.
 *
 * Show how to use the libavformat and libavcodec API to demux and
 * decode audio and video data.
 * @example demuxing_decoding.c
 */

#define __STDC_CONSTANT_MACROS
extern "C"{
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/samplefmt.h>
    #include <libavutil/timestamp.h>
    #include <libavformat/avformat.h>
    #include <libavutil/fifo.h>
}


#include <string>
#include <iostream>
#include <exception>
#include <fstream>
static AVFormatContext *fmt_ctx = NULL;
static AVCodecContext *video_dec_ctx = NULL, *audio_dec_ctx;
static int width, height;
static enum AVPixelFormat pix_fmt;
static AVStream *video_stream = NULL, *audio_stream = NULL;
static const char *src_filename = NULL;
static const char *video_dst_filename = NULL;
static const char *audio_dst_filename = NULL;
static FILE *video_dst_file = NULL;
static FILE *audio_dst_file = NULL;

static uint8_t *video_dst_data[4] = {NULL};
static int      video_dst_linesize[4];
static int video_dst_bufsize;

static int video_stream_idx = -1, audio_stream_idx = -1;
static AVFrame *frame = NULL;
static AVPacket *pkt = NULL;
static int video_frame_count = 0;
static int audio_frame_count = 0;

static int output_video_frame(AVFrame *frame)
{
    if (frame->width != width || frame->height != height ||
        frame->format != pix_fmt) {
        /* To handle this change, one could call av_image_alloc again and
         * decode the following frames into another rawvideo file. */
        fprintf(stderr, "Error: Width, height and pixel format have to be "
                "constant in a rawvideo file, but the width, height or "
                "pixel format of the input video changed:\n"
                "old: width = %d, height = %d, format = %s\n"
                "new: width = %d, height = %d, format = %s\n",
                width, height, av_get_pix_fmt_name(pix_fmt),
                frame->width, frame->height,
                av_get_pix_fmt_name(AVPixelFormat(frame->format)));
        return -1;
    }

    printf("video_frame n:%d coded_n:%d\n",
           video_frame_count++, frame->coded_picture_number);

    /* copy decoded frame to destination buffer:
     * this is required since rawvideo expects non aligned data */
    av_image_copy(video_dst_data, video_dst_linesize,
                  (const uint8_t **)(frame->data), frame->linesize,
                  pix_fmt, width, height);

    /* write to rawvideo file */
    fwrite(video_dst_data[0], 1, video_dst_bufsize, video_dst_file);
    return 0;
}

static int output_audio_frame(AVFrame *frame)
{
    char buffer[AV_TS_MAX_STRING_SIZE]{0};
    size_t unpadded_linesize = frame->nb_samples * av_get_bytes_per_sample(AVSampleFormat(frame->format));
    printf("audio_frame n:%d nb_samples:%d pts:%s\n",
           audio_frame_count++, frame->nb_samples,
           av_ts_make_time_string(buffer, frame->pts, &audio_dec_ctx->time_base));

    /* Write the raw audio data samples of the first plane. This works
     * fine for packed formats (e.g. AV_SAMPLE_FMT_S16). However,
     * most audio decoders output planar audio, which uses a separate
     * plane of audio samples for each channel (e.g. AV_SAMPLE_FMT_S16P).
     * In other words, this code will write only the first audio channel
     * in these cases.
     * You should use libswresample or libavfilter to convert the frame
     * to packed data. */
    fwrite(frame->extended_data[0], 1, unpadded_linesize, audio_dst_file);

    return 0;
}

static int decode_packet(AVCodecContext *dec, const AVPacket *pkt)
{
    int ret = 0;

    // submit the packet to the decoder
    ret = avcodec_send_packet(dec, pkt);
    if (ret < 0) {
        char buffer[AV_ERROR_MAX_STRING_SIZE]{0};
        av_make_error_string(buffer, AV_ERROR_MAX_STRING_SIZE, ret);
        fprintf(stderr, "Error submitting a packet for decoding (%s)\n", buffer);
        return ret;
    }

    // get all the available frames from the decoder
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec, frame);
        if (ret < 0) {
            // those two return values are special and mean there is no output
            // frame available, but there were no errors during decoding
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                return 0;

            char buffer[AV_ERROR_MAX_STRING_SIZE]{0};
            av_make_error_string(buffer, AV_ERROR_MAX_STRING_SIZE, ret);
            fprintf(stderr, "Error during decoding (%s)\n", buffer);
            return ret;
        }

        // write the frame data to output file
        if (dec->codec->type == AVMEDIA_TYPE_VIDEO)
            ret = output_video_frame(frame);
        else
            ret = output_audio_frame(frame);

        av_frame_unref(frame);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int open_codec_context(int *stream_idx, AVCodecContext **dec_ctx, AVFormatContext *fmt_ctx, enum AVMediaType type)
{
    int ret, stream_index;
    AVStream *st;
    const AVCodec *dec = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(type), src_filename);
        return ret;
    } else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];

        /* find decoder for the stream */
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            fprintf(stderr, "Failed to find %s codec\n",
                    av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }

        /* Allocate a codec context for the decoder */
        *dec_ctx = avcodec_alloc_context3(dec);
        if (!*dec_ctx) {
            fprintf(stderr, "Failed to allocate the %s codec context\n",
                    av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }

        /* Copy codec parameters from input stream to output codec context */
        if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                    av_get_media_type_string(type));
            return ret;
        }

        /* Init the decoders */
        if ((ret = avcodec_open2(*dec_ctx, dec, NULL)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }
        *stream_idx = stream_index;
    }

    return 0;
}

static int get_format_from_sample_fmt(const char **fmt,enum AVSampleFormat sample_fmt)
{
    int i;
    struct sample_fmt_entry {
        enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le;
    } sample_fmt_entries[] = {
        { AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
        { AV_SAMPLE_FMT_S16, "s16be", "s16le" },
        { AV_SAMPLE_FMT_S32, "s32be", "s32le" },
        { AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
        { AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
    };
    *fmt = NULL;

    for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
        struct sample_fmt_entry *entry = &sample_fmt_entries[i];
        if (sample_fmt == entry->sample_fmt) {
            *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
            return 0;
        }
    }

    fprintf(stderr,
            "sample format %s is not supported as output format\n",
            av_get_sample_fmt_name(sample_fmt));
    return -1;
}




void demuxer_decode(){
 int ret = 0;

    std::string in_file("test_video.1080p.mp4");
    std::string out_file("test_video.1080p.data");
    std::string out_video_file("test_video_encode.1080p.mp4");
    std::ofstream out_st;
    out_st.open(out_video_file,std::ios::app|std::ios::binary);
    if(!out_st.is_open()){
        std::cerr<<"open write file failed"<<std::endl;
    }

    src_filename = in_file.c_str();

    video_dst_filename = out_file.c_str();
    audio_dst_filename = NULL;

    /* open input file, and allocate format context */
    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open source file %s\n", src_filename);
        exit(1);
    }

    /* retrieve stream information */
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        exit(1);
    }

    try{

        if (open_codec_context(&video_stream_idx, &video_dec_ctx, fmt_ctx, AVMEDIA_TYPE_VIDEO) >= 0) {
            video_stream = fmt_ctx->streams[video_stream_idx];
            video_dst_file = fopen(video_dst_filename, "wb");
            if (!video_dst_file) {
                fprintf(stderr, "Could not open destination file %s\n", video_dst_filename);
                ret = 1;
            throw ;
            }
            /* allocate image where the decoded image will be put */
            width = video_dec_ctx->width;
            height = video_dec_ctx->height;
            pix_fmt = video_dec_ctx->pix_fmt;
            ret = av_image_alloc(video_dst_data, video_dst_linesize,
                                width, height, pix_fmt, 1);
            if (ret < 0) {
                fprintf(stderr, "Could not allocate raw video buffer\n");
            throw ;
            }
                video_dst_bufsize = ret;
        }

        if (open_codec_context(&audio_stream_idx, &audio_dec_ctx, fmt_ctx, AVMEDIA_TYPE_AUDIO) >= 0) {
            audio_stream = fmt_ctx->streams[audio_stream_idx];
            audio_dst_file = fopen(audio_dst_filename, "wb");
            if (!audio_dst_file) {
                fprintf(stderr, "Could not open destination file %s\n", audio_dst_filename);
                ret = 1;
            throw ;
            }
        }

        /* dump input information to stderr */
        av_dump_format(fmt_ctx, 0, src_filename, 0);

        if (!audio_stream && !video_stream) {
            fprintf(stderr, "Could not find audio or video stream in the input, aborting\n");
            ret = 1;
            throw ;
        }

        frame = av_frame_alloc();
        if (!frame) {
            fprintf(stderr, "Could not allocate frame\n");
            ret = AVERROR(ENOMEM);
            throw ;
        }

        pkt = av_packet_alloc();
        if (!pkt) {
            fprintf(stderr, "Could not allocate packet\n");
            ret = AVERROR(ENOMEM);
            throw ;
        }

        if (video_stream)
            printf("Demuxing video from file '%s' into '%s'\n", src_filename, video_dst_filename);
        if (audio_stream)
            printf("Demuxing audio from file '%s' into '%s'\n", src_filename, audio_dst_filename);



        int frame_nums = 0;
        /* read frames from the file */
        while (av_read_frame(fmt_ctx, pkt) >= 0) {
            // check if the packet belongs to a stream we are interested in, otherwise
            // skip it
            

            if (pkt->stream_index == video_stream_idx)
                ret = decode_packet(video_dec_ctx, pkt);
            else if (pkt->stream_index == audio_stream_idx)
                ret = decode_packet(audio_dec_ctx, pkt);


            //out_st.write((const char*)pkt->data,pkt->size);
            av_packet_unref(pkt);
            if (ret < 0)
                break;
            if(frame_nums>=300){
                break;
            }
            ++frame_nums;
            
        }



        /* flush the decoders */
        if (video_dec_ctx)
            decode_packet(video_dec_ctx, NULL);
        // if (audio_dec_ctx)
        //     decode_packet(audio_dec_ctx, NULL);

        printf("Demuxing succeeded.\n");

        if (video_stream) {
            printf("Play the output video file with the command:\n"
                "ffplay -f rawvideo -pix_fmt %s -video_size %dx%d %s\n",
                av_get_pix_fmt_name(pix_fmt), width, height,
                video_dst_filename);
        }

        if (audio_stream) {
            enum AVSampleFormat sfmt = audio_dec_ctx->sample_fmt;
            int n_channels = audio_dec_ctx->ch_layout.nb_channels;
            const char *fmt;

            if (av_sample_fmt_is_planar(sfmt)) {
                const char *packed = av_get_sample_fmt_name(sfmt);
                printf("Warning: the sample format the decoder produced is planar "
                    "(%s). This example will output the first channel only.\n",
                    packed ? packed : "?");
                sfmt = av_get_packed_sample_fmt(sfmt);
                n_channels = 1;
            }

            if ((ret = get_format_from_sample_fmt(&fmt, sfmt)) < 0)
                throw std::runtime_error("get_format_from_sample_fmt failed");

            printf("Play the output audio file with the command:\n"
                "ffplay -f %s -ac %d -ar %d %s\n",
                fmt, n_channels, audio_dec_ctx->sample_rate,
                audio_dst_filename);
        }

    }catch(std::exception &e){
        avcodec_free_context(&video_dec_ctx);
        avcodec_free_context(&audio_dec_ctx);
        avformat_close_input(&fmt_ctx);
        if (video_dst_file)
            fclose(video_dst_file);
        if (audio_dst_file)
            fclose(audio_dst_file);
        av_packet_free(&pkt);
        av_frame_free(&frame);
        av_free(video_dst_data[0]);
    }

}


int encode_video(){

    std::string filename("/home/liu/project/ffmpeglib/output.mp4");
    std::string outname("/home/liu/project/ffmpeglib/output-en.mp4");
	

    AVFormatContext * format_ctx = nullptr;
    AVCodecContext *dec_ctx = nullptr;
    AVFrame *decoded_frame = nullptr;
    AVPacket *pkt = nullptr;
    AVPacket *outpkt = nullptr;
    AVCodecParameters *codecPar = nullptr;
    AVFormatContext *oc = nullptr;
    const char *format = nullptr;
    AVFifo *muxing_queue = nullptr;
    AVFrame *filtered_frame = nullptr;

    format_ctx = avformat_alloc_context();
    // format_ctx->data_codec_id = AV_CODEC_ID_NONE;
    // format_ctx->video_codec_id = AV_CODEC_ID_NONE;
    // format_ctx->audio_codec_id = AV_CODEC_ID_NONE;
    // format_ctx->subtitle_codec_id = AV_CODEC_ID_NONE;
    // format_ctx->flags |= AVFMT_FLAG_NONBLOCK;

    const AVInputFormat *file_iformat = NULL;
    AVRational framerate_guessed;

    int err = 0;
    err = avformat_open_input(&format_ctx, filename.c_str(), NULL, NULL);
    err = avformat_find_stream_info(format_ctx, NULL);
    av_dump_format(format_ctx,0,filename.c_str(),0);
    //avformat_new_stream()


    const AVCodec *codec = avcodec_find_decoder(format_ctx->streams[0]->codecpar->codec_id);
    dec_ctx = avcodec_alloc_context3(codec);
    AVCodecParameters *par = format_ctx->streams[0]->codecpar;
    err = avcodec_parameters_to_context(dec_ctx, par);
    decoded_frame = av_frame_alloc();
    pkt = av_packet_alloc();
    framerate_guessed = av_guess_frame_rate(format_ctx, format_ctx->streams[0], NULL);
    codecPar = avcodec_parameters_alloc();
    err = avcodec_parameters_from_context(codecPar,dec_ctx);



    err = avformat_alloc_output_context2(&oc, NULL, format, outname.c_str());
    if (av_guess_codec(oc->oformat, NULL, oc->url, NULL, AVMEDIA_TYPE_VIDEO) == AV_CODEC_ID_NONE)
        return -1;

    int qcr = avformat_query_codec(oc->oformat, oc->oformat->video_codec, 0);
    AVStream *st = avformat_new_stream(oc, NULL);
    muxing_queue = av_fifo_alloc2(8, sizeof(AVPacket*), 0);
    filtered_frame = av_frame_alloc();
    outpkt = av_packet_alloc();


    err = avio_open2(&oc->pb, outname.c_str(), AVIO_FLAG_WRITE,&oc->interrupt_callback,NULL);

    av_dict_copy(&oc->metadata, format_ctx->metadata,AV_DICT_DONT_OVERWRITE);
    av_dict_set(&oc->metadata, "creation_time", NULL, 0);
    av_dict_set(&oc->metadata, "company_name", NULL, 0);
    av_dict_set(&oc->metadata, "product_name", NULL, 0);
    av_dict_set(&oc->metadata, "product_version", NULL, 0);
    av_dict_copy(&st->metadata, format_ctx->streams[0]->metadata, AV_DICT_DONT_OVERWRITE);

    err = avformat_write_header(oc, NULL);

    err = av_read_frame(format_ctx, outpkt);

    

    err = av_fifo_write(muxing_queue, outpkt, 1);


    if(err!=0){
        std::cerr<<"error"<<std::endl;
        return -1;
    
    }
    av_frame_unref(decoded_frame);
    av_packet_free(&pkt);
    






    try{
        //const AVCodec *codec = avcodec_find_encoder(AVCodecID::AV_CODEC_ID_H264);
        const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
        if(codec==nullptr){
            throw std::runtime_error("codec find error");
        }
    }catch(std::exception &e){
        std::cerr<<e.what()<<std::endl;

    }
    

}


void testYUV(){
    std::string filename("/home/liu/project/ffmpeglib/output.mp4");
    std::ifstream infile(filename,std::ios::binary);
    std::ofstream outfile;
    int yuv_size = 1920*1080*3/2;
    unsigned char *buffer = new unsigned char[yuv_size];
    for(int i=0;i<10;++i){
        std::string outname = std::to_string(i)+".yuv";
        outfile.open(outname,std::ios::binary);
        infile.read((char*)buffer,yuv_size);
        outfile.write((char*)buffer,yuv_size);
        outfile.close();
    }
    delete[] buffer;


}



void testDemuxer(){
    const char* inputFileUrl = "GK88_mpeg4.mp4";

	//?????????
	AVFormatContext* inputContext = nullptr;
	avformat_open_input(&inputContext, inputFileUrl, NULL, NULL);
	avformat_find_stream_info(inputContext, NULL);

	//????????????????????????
	av_dump_format(inputContext, 0, inputFileUrl,0);

	//???????????????????????????
	AVStream* audioInputStream = nullptr;
	AVStream* videoInputStream = nullptr;

	for (int i = 0; i < inputContext->nb_streams; i++)
	{
		if (inputContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
			audioInputStream = inputContext->streams[i];
		else if (inputContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
			videoInputStream = inputContext->streams[i];
		else continue;
	}

	//?????????
	const char* outputFileUrl = "test_output.mp4";
	AVFormatContext* outputContext = nullptr;
	//??????????????????????????????????????????
	avformat_alloc_output_context2(&outputContext, NULL, NULL, outputFileUrl);
	//???????????????????????????
	AVStream* videoOutputStream = avformat_new_stream(outputContext, NULL);
	//AVStream* audioOutputStream = avformat_new_stream(outputContext, NULL);
	//??????IO???
	avio_open(&outputContext->pb, outputFileUrl, AVIO_FLAG_WRITE);
	//?????????????????????????????????????????????????????????
	videoOutputStream->time_base = videoInputStream->time_base;
	avcodec_parameters_copy(videoOutputStream->codecpar, videoInputStream->codecpar);
	// //?????????????????????????????????????????????????????????
	// audioOutputStream->time_base = audioInputStream->time_base;
	// avcodec_parameters_copy(audioOutputStream->codecpar, audioInputStream->codecpar);
	//?????????????????????
	avformat_write_header(outputContext, NULL);
	av_dump_format(outputContext, 0, outputFileUrl, 1);

	//???????????????
	AVPacket packet;
    int frame_num = 0;
    bool hasKey = false;
	for (;;)
	{
		int result = av_read_frame(inputContext, &packet);
		if (result != 0) break;
        if(++frame_num<100) continue;
        if(frame_num >= 10000) break;
        if(!hasKey){
            if(!(packet.flags&AV_PKT_FLAG_KEY)){
                continue;
            }else{
                hasKey = true;
            }
        }
        


		if (packet.stream_index == videoOutputStream->index)
		{
			std::cout << "??????:";
		}
		// else if (packet.stream_index == audioOutputStream->index) {
		// 	std::cout << "??????:";
		// }
		std::cout << packet.pts << " : " << packet.dts << " :" << packet.size <<std::endl;

		av_interleaved_write_frame(outputContext, &packet);
	}
    std::cout<<"frame sum :"<<frame_num<<std::endl;
	av_write_trailer(outputContext);
	avformat_close_input(&inputContext);
	avio_closep(&outputContext->pb);
	avformat_free_context(outputContext);
	outputContext = nullptr;
}

int main (int argc, char **argv)
{
    //testYUV();
    //demuxer_decode();
    //encode_video();
    testDemuxer();
    return 0;
}
