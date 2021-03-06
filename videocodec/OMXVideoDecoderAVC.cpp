/*
* Copyright (c) 2009-2011 Intel Corporation.  All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/


#include "OMXVideoDecoderAVC.h"

// Be sure to have an equal string in VideoDecoderHost.cpp (libmix)
static const char* AVC_MIME_TYPE = "video/h264";
#define INVALID_PTS (OMX_S64)-1


OMXVideoDecoderAVC::OMXVideoDecoderAVC()
    : mAccumulateBuffer(NULL),
      mBufferSize(0),
      mFilledLen(0),
      mTimeStamp(INVALID_PTS) {
    omx_verboseLog("OMXVideoDecoderAVC is constructed.");
    mVideoDecoder = createVideoDecoder(AVC_MIME_TYPE);
    if (!mVideoDecoder) {
        omx_errorLog("createVideoDecoder failed for \"%s\"", AVC_MIME_TYPE);
    }
    BuildHandlerList();
}

OMXVideoDecoderAVC::~OMXVideoDecoderAVC() {
    omx_verboseLog("OMXVideoDecoderAVC is destructed.");
}

OMX_ERRORTYPE OMXVideoDecoderAVC::InitInputPortFormatSpecific(OMX_PARAM_PORTDEFINITIONTYPE *paramPortDefinitionInput) {
    //OMX_VIDEO_PARAM_INTEL_AVC_DECODE_SETTINGS
    memset(&mDecodeSettings, 0, sizeof(mDecodeSettings));
    SetTypeHeader(&mDecodeSettings, sizeof(mDecodeSettings));
    mDecodeSettings.nMaxNumberOfReferenceFrame = NUM_REFERENCE_FRAME;

    // OMX_PARAM_PORTDEFINITIONTYPE
    paramPortDefinitionInput->nBufferCountActual = INPORT_ACTUAL_BUFFER_COUNT;
    paramPortDefinitionInput->nBufferCountMin = INPORT_MIN_BUFFER_COUNT;
    paramPortDefinitionInput->nBufferSize = INPORT_BUFFER_SIZE;
    paramPortDefinitionInput->format.video.cMIMEType = (OMX_STRING)AVC_MIME_TYPE;
    paramPortDefinitionInput->format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;

    // OMX_VIDEO_PARAM_AVCTYPE
    memset(&mParamAvc, 0, sizeof(mParamAvc));
    SetTypeHeader(&mParamAvc, sizeof(mParamAvc));
    mParamAvc.nPortIndex = INPORT_INDEX;
    // TODO: check eProfile/eLevel
    mParamAvc.eProfile = OMX_VIDEO_AVCProfileHigh; //OMX_VIDEO_AVCProfileBaseline;
    mParamAvc.eLevel = OMX_VIDEO_AVCLevel41; //OMX_VIDEO_AVCLevel1;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVC::ProcessorInit(void *parser_handle) {
    return OMXVideoDecoderBase::ProcessorInit(parser_handle);
}

OMX_ERRORTYPE OMXVideoDecoderAVC::ProcessorDeinit(void) {
    if (mAccumulateBuffer) {
        delete mAccumulateBuffer;
    }
    mAccumulateBuffer = NULL;
    mBufferSize = 0;
    mFilledLen = 0;
    mTimeStamp = INVALID_PTS;

    return OMXVideoDecoderBase::ProcessorDeinit();
}

OMX_ERRORTYPE OMXVideoDecoderAVC::ProcessorFlush(OMX_U32 portIndex) {
    mFilledLen = 0;
    mTimeStamp = INVALID_PTS;
    return OMXVideoDecoderBase::ProcessorFlush(portIndex);
}

OMX_ERRORTYPE OMXVideoDecoderAVC::ProcessorProcess(
        OMX_BUFFERHEADERTYPE **buffers,
        buffer_retain_t *retains,
        OMX_U32 numberBuffers) {

    return OMXVideoDecoderBase::ProcessorProcess(buffers, retains, numberBuffers);
}

OMX_ERRORTYPE OMXVideoDecoderAVC::PrepareConfigBuffer(VideoConfigBuffer *p) {
    OMX_ERRORTYPE ret;
    ret = OMXVideoDecoderBase::PrepareConfigBuffer(p);
    CHECK_RETURN_VALUE("OMXVideoDecoderBase::PrepareConfigBuffer");

    omx_warnLog("AVC Video decoder used in Video Conferencing Mode.");

    // For video conferencing application
    p->width = mDecodeSettings.nMaxWidth;
    p->height = mDecodeSettings.nMaxHeight;
    p->profile = VAProfileH264High;
    p->surfaceNumber =  0; //mDecodeSettings.nMaxNumberOfReferenceFrame + EXTRA_REFERENCE_FRAME;
    p->flag = USE_NATIVE_GRAPHIC_BUFFER;

    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVC::PrepareDecodeBuffer(OMX_BUFFERHEADERTYPE *buffer, buffer_retain_t *retain, VideoDecodeBuffer *p) {
    OMX_ERRORTYPE ret;
    ret = OMXVideoDecoderBase::PrepareDecodeBuffer(buffer, retain, p);
    CHECK_RETURN_VALUE("OMXVideoDecoderBase::PrepareDecodeBuffer");

    // OMX_BUFFERFLAG_CODECCONFIG is an optional flag
    // if flag is set, buffer will only contain codec data.
    if (buffer->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
        omx_verboseLog("Received AVC codec data.");
        return ret;
    }

    // OMX_BUFFERFLAG_ENDOFFRAME is an optional flag
    if (buffer->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) {
        // TODO: if OMX_BUFFERFLAG_ENDOFFRAME indicates end of a NAL unit and in OMXVideoDecodeBase
        // we set buffer flag to HAS_COMPLETE_FRAME,  corruption will happen
        mTimeStamp = buffer->nTimeStamp;
        if (mFilledLen == 0) {
            // buffer is not accumulated and it contains a complete frame
            return ret;
        }
        // buffer contains  the last part of fragmented frame
        ret = AccumulateBuffer(buffer);
        CHECK_RETURN_VALUE("AccumulateBuffer");
        ret = FillDecodeBuffer(p);
        CHECK_RETURN_VALUE("FillDecodeBuffer");
        return ret;
    }

    omx_warnLog("Received fragmented buffer.");
    // use time stamp to determine frame boundary
    if (mTimeStamp == INVALID_PTS) {
        // first ever buffer
        mTimeStamp = buffer->nTimeStamp;
    }

    if (mTimeStamp != buffer->nTimeStamp && mFilledLen != 0) {
        // buffer accumulated contains a complete frame
        ret = FillDecodeBuffer(p);
        CHECK_RETURN_VALUE("FillDecodeBuffer");
        // retain the current buffer
        *retain = BUFFER_RETAIN_GETAGAIN;
    } else {
        // buffer accumulation for beginning of fragmented buffer (mFilledLen == 0) or
        // middle/end of fragmented buffer (mFilledLen != 0)
        ret = AccumulateBuffer(buffer);
        CHECK_RETURN_VALUE("AccumulateBuffer");
        ret = OMX_ErrorNotReady;
    }

    if (buffer->nFilledLen != 0) {
        mTimeStamp = buffer->nTimeStamp;
    }
    return ret;
}

OMX_ERRORTYPE OMXVideoDecoderAVC::AccumulateBuffer(OMX_BUFFERHEADERTYPE *buffer) {
    // check if allocated buffer is big enough
    if (mFilledLen + buffer->nFilledLen > mBufferSize) {
        mBufferSize = mFilledLen + buffer->nFilledLen;
        if (mBufferSize < INPORT_BUFFER_SIZE) {
            mBufferSize = INPORT_BUFFER_SIZE;
        }
        if (mBufferSize == 0) {
            return OMX_ErrorBadParameter;
        }
        OMX_U8 *temp = new OMX_U8 [mBufferSize];
        if (temp == NULL) {
            mBufferSize = 0;
            return OMX_ErrorInsufficientResources;
        }
        if (mFilledLen != 0) {
            memcpy(temp, mAccumulateBuffer, mFilledLen);
        }
        if (mAccumulateBuffer) {
            delete [] mAccumulateBuffer;
        }
        mAccumulateBuffer = temp;
    }
    if (buffer->nFilledLen != 0) {
        memcpy(mAccumulateBuffer + mFilledLen, buffer->pBuffer + buffer->nOffset, buffer->nFilledLen);
	mBufferSize=0;
    }
    mFilledLen += buffer->nFilledLen;
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVC::FillDecodeBuffer(VideoDecodeBuffer *p) {
    p->data = mAccumulateBuffer;
    p->size = mFilledLen;
    p->timeStamp = mTimeStamp;
    p->flag = HAS_COMPLETE_FRAME;

    mFilledLen = 0;
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVC::BuildHandlerList(void) {
    OMXVideoDecoderBase::BuildHandlerList();
    AddHandler(OMX_IndexParamVideoAvc, GetParamVideoAvc, SetParamVideoAvc);
    AddHandler((OMX_INDEXTYPE)OMX_IndexParamIntelAVCDecodeSettings, GetParamIntelAVCDecodeSettings, SetParamIntelAVCDecodeSettings);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVC::GetParamVideoAvc(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_AVCTYPE *p = (OMX_VIDEO_PARAM_AVCTYPE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, INPORT_INDEX);

    memcpy(p, &mParamAvc, sizeof(*p));
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVC::SetParamVideoAvc(OMX_PTR pStructure) {
    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_AVCTYPE *p = (OMX_VIDEO_PARAM_AVCTYPE *)pStructure;
    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, INPORT_INDEX);
    CHECK_SET_PARAM_STATE();

    // TODO: do we need to check if port is enabled?
    // TODO: see SetPortAvcParam implementation - Can we make simple copy????
    memcpy(&mParamAvc, p, sizeof(mParamAvc));
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMXVideoDecoderAVC::GetParamIntelAVCDecodeSettings(OMX_PTR pStructure) {
    return OMX_ErrorNotImplemented;
}

OMX_ERRORTYPE OMXVideoDecoderAVC::SetParamIntelAVCDecodeSettings(OMX_PTR pStructure) {
    omx_warnLog("SetParamIntelAVCDecodeSettings");

    OMX_ERRORTYPE ret;
    OMX_VIDEO_PARAM_INTEL_AVC_DECODE_SETTINGS *p = (OMX_VIDEO_PARAM_INTEL_AVC_DECODE_SETTINGS *)pStructure;

    CHECK_TYPE_HEADER(p);
    CHECK_PORT_INDEX(p, INPORT_INDEX);
    CHECK_SET_PARAM_STATE();

    if(p->nMaxNumberOfReferenceFrame == 0) {
        // TODO: check if we just return in this case.
        p->nMaxNumberOfReferenceFrame = NUM_REFERENCE_FRAME;
    }
    omx_infoLog("Maximum width = %lu, height = %lu, dpb = %lu", p->nMaxWidth, p->nMaxHeight, p->nMaxNumberOfReferenceFrame);
    mDecodeSettings = *p;

    return OMX_ErrorNone;
}


DECLARE_OMX_COMPONENT("OMX.Intel.VideoDecoder.AVC", "video_decoder.avc", OMXVideoDecoderAVC);
