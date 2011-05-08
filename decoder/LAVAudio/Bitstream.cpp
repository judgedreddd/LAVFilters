/*
 *      Copyright (C) 2011 Hendrik Leppkes
 *      http://www.1f0.de
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 */

#include "stdafx.h"
#include "LAVAudio.h"

#include <MMReg.h>
#include "moreuuids.h"

#define LAV_BITSTREAM_BUFFER_SIZE 4096

static struct {
  CodecID codec;
  BitstreamCodecs config;
} lavf_bitstream_config[] = {
  { CODEC_ID_AC3,    BS_AC3 },
  { CODEC_ID_EAC3,   BS_EAC3 },
  { CODEC_ID_TRUEHD, BS_TRUEHD },
  { CODEC_ID_DTS,    BS_DTS } // DTS-HD is still DTS, and handled special below
};

// Check wether a codec is bitstreaming eligible and enabled
BOOL CLAVAudio::IsBitstreaming(CodecID codec)
{
  for(int i = 0; i < countof(lavf_bitstream_config); ++i) {
    if (lavf_bitstream_config[i].codec == codec) {
      return m_settings.bBitstream[lavf_bitstream_config[i].config];
    }
  }
  return FALSE;
}

HRESULT CLAVAudio::InitBitstreaming()
{
  // Alloc buffer for the AVIO context
  BYTE *buffer = (BYTE *)CoTaskMemAlloc(LAV_BITSTREAM_BUFFER_SIZE + FF_INPUT_BUFFER_PADDING_SIZE);
  if(!buffer)
    return E_FAIL;
  
  // Create AVIO context
  m_avioBitstream = avio_alloc_context(buffer, LAV_BITSTREAM_BUFFER_SIZE, 1, this, NULL, BSWriteBuffer, NULL);
  if(!m_avioBitstream) {
    SAFE_CO_FREE(buffer);
    return E_FAIL;
  }

  return S_OK;
}

HRESULT CLAVAudio::ShutdownBitstreaming()
{
  if (m_avioBitstream) {
    SAFE_CO_FREE(m_avioBitstream->buffer);
    av_freep(&m_avioBitstream);
  }
  return S_OK;
}

// Static function for the AVIO context that writes the buffer into our own output buffer
int CLAVAudio::BSWriteBuffer(void *opaque, uint8_t *buf, int buf_size)
{
  CLAVAudio *filter = (CLAVAudio *)opaque;
  filter->m_bsOutput.Append(buf, buf_size);
  return buf_size;
}

HRESULT CLAVAudio::CreateBitstreamContext(CodecID codec, WAVEFORMATEX *wfe)
{
  int ret = 0;

  if (m_avBSContext)
    FreeBitstreamContext();
  m_bsParser.Reset();

  m_pParser = av_parser_init(codec);
  ASSERT(m_pParser);

  m_pAVCtx = avcodec_alloc_context();
  CheckPointer(m_pAVCtx, E_POINTER);

  DbgLog((LOG_TRACE, 20, "Creating Bistreaming Context..."));

  m_avBSContext = avformat_alloc_output_context("spdif", NULL, NULL);
  if (!m_avBSContext) {
    DbgLog((LOG_ERROR, 10, L"::CreateBitstreamContext() -- alloc of avformat spdif muxer failed"));
    goto fail;
  }

  m_avBSContext->pb = m_avioBitstream;
  m_avBSContext->oformat->flags |= AVFMT_NOFILE;

  // DTS-HD is by default off, unless explicitly asked for
  if (m_settings.DTSHDFraming && m_settings.bBitstream[BS_DTSHD]) {
    m_bDTSHD = TRUE;
    av_set_string3(m_avBSContext->priv_data, "dtshd_rate", "768000", 0, NULL);
  } else {
    m_bDTSHD = FALSE;
    av_set_string3(m_avBSContext->priv_data, "dtshd_rate", "0", 0, NULL);
  }
  av_set_string3(m_avBSContext->priv_data, "dtshd_fallback_time", "-1", 0, NULL);

  AVStream *st = av_new_stream(m_avBSContext, 0);
  if (!st) {
    DbgLog((LOG_ERROR, 10, L"::CreateBitstreamContext() -- alloc of output stream failed"));
    goto fail;
  }
  m_pAVCtx->codec_id    = st->codec->codec_id    = codec;
  m_pAVCtx->codec_type  = st->codec->codec_type  = AVMEDIA_TYPE_AUDIO;
  m_pAVCtx->channels    = st->codec->channels    = wfe->nChannels;
  m_pAVCtx->sample_rate = st->codec->sample_rate = wfe->nSamplesPerSec;

  ret = av_write_header(m_avBSContext);
  if (ret < 0) {
    DbgLog((LOG_ERROR, 10, L"::CreateBitstreamContext() -- av_write_header returned an error code (%d)", -ret));
    goto fail;
  }

  m_nCodecId = codec;

  return S_OK;
fail:
  FreeBitstreamContext();
  return E_FAIL;
}

HRESULT CLAVAudio::UpdateBitstreamContext()
{
  if (!m_pInput || !m_pInput->IsConnected())
    return E_UNEXPECTED;

  BOOL bBitstream = IsBitstreaming(m_nCodecId);
  if ((bBitstream && !m_avBSContext) || (!bBitstream && m_avBSContext)) {
    CMediaType mt = m_pInput->CurrentMediaType();

    const void *format = mt.Format();
    GUID format_type = mt.formattype;

    // Override the format type
    if (mt.subtype == MEDIASUBTYPE_FFMPEG_AUDIO && format_type == FORMAT_WaveFormatExFFMPEG) {
      WAVEFORMATEXFFMPEG *wfexff = (WAVEFORMATEXFFMPEG *)mt.Format();
      format = &wfexff->wfex;
      format_type = FORMAT_WaveFormatEx;
    }

    ffmpeg_init(m_nCodecId, format, format_type);
    m_bQueueResync = TRUE;
  }

  // Configure DTS-HD setting
  if(m_avBSContext) {
    if (m_settings.bBitstream[BS_DTSHD] && m_settings.DTSHDFraming) {
      m_bDTSHD = TRUE;
      av_set_string3(m_avBSContext->priv_data, "dtshd_rate", "768000", 0, NULL);
    } else {
      m_bDTSHD = FALSE; // Force auto-detection
      av_set_string3(m_avBSContext->priv_data, "dtshd_rate", "0", 0, NULL);
    }
  }

  return S_OK;
}

HRESULT CLAVAudio::FreeBitstreamContext()
{
  if (m_avBSContext)
    avformat_free_context(m_avBSContext);
  m_avBSContext = NULL;

  if (m_pParser)
    av_parser_close(m_pParser);
  m_pParser = NULL;

  if (m_pAVCtx) {
    avcodec_close(m_pAVCtx);
    av_free(m_pAVCtx->extradata);
    av_free(m_pAVCtx);
    m_pAVCtx = NULL;
  }

  return S_OK;
}

CMediaType CLAVAudio::CreateBitstreamMediaType(CodecID codec)
{
   CMediaType mt;

   mt.majortype  = MEDIATYPE_Audio;
   mt.subtype    = MEDIASUBTYPE_PCM;
   mt.formattype = FORMAT_WaveFormatEx;

   WAVEFORMATEXTENSIBLE wfex;
   memset(&wfex, 0, sizeof(wfex));

   WAVEFORMATEX* wfe = &wfex.Format;

   wfe->nChannels = 2;
   wfe->wBitsPerSample = 16;

   GUID subtype = GUID_NULL;

   switch(codec) {
   case CODEC_ID_AC3:
     wfe->wFormatTag     = WAVE_FORMAT_DOLBY_AC3_SPDIF;
     wfe->nSamplesPerSec = 48000;
     break;
   case CODEC_ID_EAC3:
     wfe->nSamplesPerSec = 192000;
     wfe->nChannels      = 2;
     subtype = KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL_PLUS;
     break;
   case CODEC_ID_TRUEHD:
     wfe->nSamplesPerSec = 192000;
     wfe->nChannels      = 8;
     subtype = KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP;
     break;
   case CODEC_ID_DTS:
     if (m_settings.bBitstream[BS_DTSHD] && m_bDTSHD) {
       wfe->nSamplesPerSec = 192000;
       wfe->nChannels      = 8;
       subtype = KSDATAFORMAT_SUBTYPE_IEC61937_DTS_HD;
     } else {
       wfe->wFormatTag     = WAVE_FORMAT_DOLBY_AC3_SPDIF; // huh? but it works.
       wfe->nSamplesPerSec = 48000;
     }
     break;
   default:
     ASSERT(0);
     break;
   }

   wfe->nBlockAlign = wfe->nChannels * wfe->wBitsPerSample / 8;
   wfe->nAvgBytesPerSec = wfe->nSamplesPerSec * wfe->nBlockAlign;

   if (subtype != GUID_NULL) {
      wfex.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
      wfex.Format.cbSize = sizeof(wfex) - sizeof(wfex.Format);
      wfex.dwChannelMask = get_channel_mask(wfe->nChannels);
      wfex.Samples.wValidBitsPerSample = wfex.Format.wBitsPerSample;
      wfex.SubFormat = subtype;
   }

   mt.SetSampleSize(1);
   mt.SetFormat((BYTE*)&wfex, sizeof(wfex.Format) + wfex.Format.cbSize);

   return mt;
}

void CLAVAudio::ActivateDTSHDMuxing()
{
  m_bDTSHD = TRUE;
  av_set_string3(m_avBSContext->priv_data, "dtshd_rate", "768000", 0, NULL);
}

HRESULT CLAVAudio::Bitstream(const BYTE *p, int buffsize, int &consumed)
{
  int ret = 0;
  const BYTE *pDataInBuff = p;
  BOOL bEOF = (buffsize == -1);
  if (buffsize == -1) buffsize = 1;

  AVPacket avpkt;
  av_init_packet(&avpkt);
  avpkt.duration = 1;

  ASSERT(pDataInBuff || bEOF);

  consumed = 0;
  while (buffsize > 0) {
    if (bEOF) buffsize = 0;
    else {
      if (buffsize+FF_INPUT_BUFFER_PADDING_SIZE > m_nFFBufferSize) {
        m_nFFBufferSize = buffsize + FF_INPUT_BUFFER_PADDING_SIZE;
        m_pFFBuffer = (BYTE*)realloc(m_pFFBuffer, m_nFFBufferSize);
      }

      memcpy(m_pFFBuffer, pDataInBuff, buffsize);
      memset(m_pFFBuffer+buffsize, 0, FF_INPUT_BUFFER_PADDING_SIZE);
    }

    BYTE *pOut = NULL;
    int pOut_size = 0;
    int used_bytes = av_parser_parse2(m_pParser, m_pAVCtx, &pOut, &pOut_size, m_pFFBuffer, buffsize, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (used_bytes < 0) {
      return E_FAIL;
    } else if(used_bytes == 0 && pOut_size == 0) {
      DbgLog((LOG_TRACE, 50, L"::Bitstream() - could not process buffer, starving?"));
      break;
    }

    // Timestamp cache to compensate for one frame delay the parser might introduce, in case the frames were already perfectly sliced apart
    // If we used more (or equal) bytes then was output again, we encountered a new frame, update timestamps
    if (used_bytes >= pOut_size) {
      m_rtStartInputCache = m_rtStartInput;
      m_rtStopInputCache = m_rtStopInput;
    }

    if (!bEOF && used_bytes > 0) {
      buffsize -= used_bytes;
      pDataInBuff += used_bytes;
      consumed += used_bytes;
    }

    if (pOut_size > 0) {
      m_bsParser.Parse(m_nCodecId, pOut, pOut_size, m_pParser->priv_data);
      if (m_nCodecId == CODEC_ID_DTS && !m_bDTSHD && m_bsParser.m_bDTSHD && m_settings.bBitstream[BS_DTSHD]) {
        ActivateDTSHDMuxing();
      }

      avpkt.data = (uint8_t *)pOut;
      avpkt.size = pOut_size;

      // Write SPDIF muxed frame
      ret = av_write_frame(m_avBSContext, &avpkt);
      if(ret < 0) {
        DbgLog((LOG_ERROR, 20, "::Bitstream(): av_write_frame returned error code (%d)", -ret));
        return E_FAIL;
      }

      // Deliver frame
      if (m_bsOutput.GetCount() > 0) {
        DeliverBitstream(m_nCodecId, m_bsOutput.Ptr(), m_bsOutput.GetCount(), pOut_size, m_rtStartInputCache, m_rtStopInputCache);
        m_bsOutput.SetSize(0);
      }
    }
  }

  return S_OK;
}

HRESULT CLAVAudio::DeliverBitstream(CodecID codec, const BYTE *buffer, DWORD dwSize, DWORD dwFrameSize, REFERENCE_TIME rtStartInput, REFERENCE_TIME rtStopInput)
{
  HRESULT hr = S_OK;

  CMediaType mt = CreateBitstreamMediaType(codec);
  WAVEFORMATEX* wfe = (WAVEFORMATEX*)mt.Format();

  if(FAILED(hr = ReconnectOutput(dwSize, mt))) {
    return hr;
  }

  IMediaSample *pOut;
  BYTE *pDataOut = NULL;
  if(FAILED(GetDeliveryBuffer(&pOut, &pDataOut))) {
    return E_FAIL;
  }

  if(hr == S_OK) {
    DbgLog((LOG_CUSTOM1, 1, L"Sending new Media Type"));
    m_pOutput->SetMediaType(&mt);
    pOut->SetMediaType(&mt);
  }

  REFERENCE_TIME rtStart = m_rtStart, rtStop = AV_NOPTS_VALUE, rtDur = AV_NOPTS_VALUE;
  double dDuration = 0;
  if (codec == CODEC_ID_TRUEHD || codec == CODEC_ID_EAC3 || (codec == CODEC_ID_DTS && m_bDTSHD)) {
    if (rtStartInput != AV_NOPTS_VALUE && rtStopInput != AV_NOPTS_VALUE) {
      rtStart = rtStartInput;
      rtDur = rtStopInput - rtStartInput;
    } else {
      dDuration = DBL_SECOND_MULT * dwSize / wfe->nBlockAlign / wfe->nSamplesPerSec;
    }
  } else if (m_bsParser.m_dwBitRate <= 1 && rtStartInput != AV_NOPTS_VALUE && rtStopInput != AV_NOPTS_VALUE) {
    rtDur = rtStopInput - rtStartInput;
  } else {
    if (m_bsParser.m_dwBlocks) { // Used by DTS
      const DWORD dwBlocks32 = m_bsParser.m_dwBlocks * 32;
      const DWORD dwBlocks = (m_bsParser.m_dwFrameSize + dwBlocks32 - 1) / dwBlocks32;
      dDuration = DBL_SECOND_MULT * dwBlocks * dwBlocks32 * 8 / m_bsParser.m_dwBitRate;
    } else { // AC-3
      dDuration = DBL_SECOND_MULT * dwFrameSize * 8 / m_bsParser.m_dwBitRate;
    }
  }
  if (rtDur == AV_NOPTS_VALUE) {
    rtDur = (REFERENCE_TIME)(dDuration + 0.5);
    m_dStartOffset += fmod(dDuration, 1.0);

    m_rtStart = rtStart + (REFERENCE_TIME)dDuration;

    if (m_dStartOffset > 0.5) {
      m_rtStart++;
      m_dStartOffset -= 1.0;
    }
  } else {
    m_rtStart = rtStart + rtDur;
  }

  rtStop = rtStart + rtDur;

  pOut->SetTime(&rtStart, &rtStop);
  pOut->SetMediaTime(NULL, NULL);

  pOut->SetPreroll(FALSE);
  pOut->SetDiscontinuity(m_bDiscontinuity);
  m_bDiscontinuity = FALSE;
  pOut->SetSyncPoint(TRUE);

  pOut->SetActualDataLength(dwSize);

  memcpy(pDataOut, buffer, dwSize);

  hr = m_pOutput->Deliver(pOut);

  SafeRelease(&pOut);
  return hr;
}
