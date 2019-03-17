#include "AmB2BMedia.h"
#include "AmAudio.h"
#include "AmB2BSession.h"
#include "AmRtpReceiver.h"
#include "AmUtils.h"
#include "sip/msg_logger.h"
#include "amci/codecs.h"

#include <string.h>
#include <strings.h>
#include <algorithm>
#include <stdexcept>

using namespace std;

#define TRACE DBG
#define UNDEFINED_PAYLOAD (-1)

/** class for computing payloads for relay the simpliest way - allow relaying of
 * all payloads supported by remote party */
static B2BMediaStatistics b2b_stats;

static const string zero_ip("0.0.0.0");

static void replaceRtcpAttr(SdpMedia &m, const string& relay_address, int rtcp_port)
{
    for(auto &a : m.attributes) {
        try {
            if (a.attribute == "rtcp") {
                RtcpAddress addr(a.value);
                addr.setPort(rtcp_port);
                if (addr.hasAddress()) addr.setAddress(relay_address);
                a.value = addr.print();
            }
        } catch (const exception &e) {
            DBG("can't replace RTCP address: %s\n", e.what());
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////

void B2BMediaStatistics::incCodecWriteUsage(const string &codec_name)
{
    if (codec_name.empty()) return;

    AmLock lock(mutex);

    map<string, int>::iterator i = codec_write_usage.find(codec_name);

    if (i != codec_write_usage.end()) i->second++;
    else codec_write_usage[codec_name] = 1;
}

void B2BMediaStatistics::decCodecWriteUsage(const string &codec_name)
{
    if (codec_name.empty()) return;

    AmLock lock(mutex);
    map<string, int>::iterator i = codec_write_usage.find(codec_name);
    if (i != codec_write_usage.end()) {
        if (i->second > 0) i->second--;
    }
}

void B2BMediaStatistics::incCodecReadUsage(const string &codec_name)
{
    if (codec_name.empty()) return;

    AmLock lock(mutex);

    map<string, int>::iterator i = codec_read_usage.find(codec_name);

    if (i != codec_read_usage.end()) i->second++;
    else codec_read_usage[codec_name] = 1;
}

void B2BMediaStatistics::decCodecReadUsage(const string &codec_name)
{
    if (codec_name.empty()) return;

    AmLock lock(mutex);

    map<string, int>::iterator i = codec_read_usage.find(codec_name);
    if (i != codec_read_usage.end()) {
        if (i->second > 0) i->second--;
    }
}

B2BMediaStatistics *B2BMediaStatistics::instance()
{
    return &b2b_stats;
}

void B2BMediaStatistics::reportCodecWriteUsage(string &dst)
{
    if (codec_write_usage.empty()) {
        dst = "pcma=0"; // to be not empty
        return;
    }

    bool first = true;
    dst.clear();
    AmLock lock(mutex);
    for (map<string, int>::iterator i = codec_write_usage.begin();
         i != codec_write_usage.end(); ++i)
    {
        if (first) first = false;
        else dst += ",";
        dst += i->first;
        dst += "=";
        dst += int2str(i->second);
    }
}

void B2BMediaStatistics::reportCodecReadUsage(string &dst)
{
    if (codec_read_usage.empty()) {
        dst = "pcma=0"; // to be not empty
        return;
    }

    bool first = true;
    dst.clear();
    AmLock lock(mutex);
    for (map<string, int>::iterator i = codec_read_usage.begin();
         i != codec_read_usage.end(); ++i)
    {
        if (first) first = false;
        else dst += ",";
        dst += i->first;
        dst += "=";
        dst += int2str(i->second);
    }
}

void B2BMediaStatistics::getReport(const AmArg &, AmArg &ret)
{
    AmArg write_usage;
    AmArg read_usage;

    { // locked area
        AmLock lock(mutex);

        for (map<string, int>::iterator i = codec_write_usage.begin();
             i != codec_write_usage.end(); ++i)
        {
            AmArg avp;
            avp["codec"] = i->first;
            avp["count"] = i->second;
            write_usage.push(avp);
        }

        for (map<string, int>::iterator i = codec_read_usage.begin();
             i != codec_read_usage.end(); ++i)
        {
            AmArg avp;
            avp["codec"] = i->first;
            avp["count"] = i->second;
            read_usage.push(avp);
        }
    }

    ret["write"] = write_usage;
    ret["read"] = read_usage;
}

//////////////////////////////////////////////////////////////////////////////////

void AudioStreamData::initialize(AmB2BSession *session)
{
    stream = new AmRtpAudio(session, session->getRtpInterface(), session->getRtpAddr());

    stream->setRtpRelayTransparentSeqno(session->getRtpRelayTransparentSeqno());
    stream->setRtpRelayTransparentSSRC(session->getRtpRelayTransparentSSRC());
    stream->setRtpRelayFilterRtpDtmf(session->getEnableDtmfRtpFiltering());
    stream->setRtpForceRelayDtmf(session->getEnableDtmfForceRelay());
    stream->setRtpForceRelayCN(session->getEnableCNForceRelay());
    stream->setRtpTimeout(session->getRtpTimeout());
    stream->setSymmetricRtpEndless(session->getRtpEndlessSymmetricRtp());
    stream->setSymmetricRtpIgnoreRTCP(session->getRtpSymmetricRtpIgnoreRTCP());
    stream->setRtpPing(session->getRtpPing());
    stream->setRtpRelayTimestampAligning(session->getRtpRelayTimestampAligning());

    TransProt trsp = session->getMediaTransport();
    if(TP_NONE!=trsp) stream->setTransport(trsp);

    if(session->getEnableDtmfRtpDetection())
        stream->force_receive_dtmf = true;

    force_symmetric_rtp = session->getRtpRelayForceSymmetricRtp();
    enable_dtmf_transcoding = session->getEnableDtmfTranscoding();
    enable_inbound_dtmf_filtering = session->getEnableInboundDtmfFiltering();

    session->getLowFiPLs(lowfi_payloads);
    stream->setLocalIP(session->localMediaIP());
}

AudioStreamData::AudioStreamData(AmB2BSession *session)
  : shared_stream(false),
    in(nullptr), out(nullptr),
    initialized(false),
    force_symmetric_rtp(false),
    enable_dtmf_transcoding(false),
    dtmf_detector(nullptr),
    dtmf_queue(nullptr),
    relay_enabled(false), relay_port(0),
    relay_paused(false),
    muted(false),
    receiving(true),
    outgoing_payload(UNDEFINED_PAYLOAD),
    incoming_payload(UNDEFINED_PAYLOAD)
{
    if(session) initialize(session);
    else stream = nullptr; // not initialized yet
}

void AudioStreamData::changeSession(AmB2BSession *session)
{
    if(!stream) {
        // the stream was not created yet
        TRACE("delayed stream initialization for session %p", static_cast<void *>(session));
        if (session) initialize(session);
    } else {
        // the stream is already created

        if (session) {
            stream->changeSession(session);

        /* FIXME: do we want to reinitialize the stream?
        stream->setRtpRelayTransparentSeqno(session->getRtpRelayTransparentSeqno());
        stream->setRtpRelayTransparentSSRC(session->getRtpRelayTransparentSSRC());
        force_symmetric_rtp = session->getRtpRelayForceSymmetricRtp();
        enable_dtmf_transcoding = session->getEnableDtmfTranscoding();
        session->getLowFiPLs(lowfi_payloads);
        stream->setLocalIP(session->localMediaIP());
        ...
        }*/
        } else {
            clear(); // free the stream and other stuff because it can't be used anyway
        }
    }
}

void AudioStreamData::clear()
{
    resetStats();
    if(in) {
        //in->close();
        //delete in;
        in = nullptr;
    }

    if(stream) {
        if(!shared_stream) {
            delete stream;
        } else {
            //cleanup relay for shared stream
            stream->disableRtpRelay();
            stream->setRelayStream(nullptr);
        }
        stream = nullptr;
    }
    clearDtmfSink();
    initialized = false;
}

void AudioStreamData::stopStreamProcessing()
{
    if(stream) stream->stopReceiving();
}

void AudioStreamData::resumeStreamProcessing()
{
    if(stream) stream->resumeReceiving();
}

void AudioStreamData::setRelayStream(AmRtpAudio *other)
{
    if(!stream) return;

    if(relay_address.empty()) {
        DBG("not setting relay for empty relay address");
        stream->disableRtpRelay();
        return;
    }

    if(relay_enabled && other) {
        if(other->getFrameTime() == stream->getFrameTime()) {
            stream->setRelayStream(other);
            stream->setForceBuffering(other->isRecordEnabled());
            stream->setRelayPayloads(relay_mask);
            stream->setRelayPayloadMap(relay_map);
            if (!relay_paused)
                stream->enableRtpRelay();
            stream->setRAddr(relay_address,
                static_cast<unsigned short>(relay_port),
                static_cast<unsigned short>(relay_port+1));
        } else {
            DBG("not setting relay for streams with different frame sizes");
        }
    } else {
        // nothing to relay or other stream not set
        stream->disableRtpRelay();
    }
}

void AudioStreamData::setRelayPayloads(const SdpMedia &m, RelayController *ctrl)
{
    ctrl->computeRelayMask(m, relay_enabled, relay_mask, relay_map);
}

void AudioStreamData::setRelayDestination(const string& connection_address, int port)
{
    relay_address = connection_address; relay_port = port;
}

void AudioStreamData::setRelayPaused(bool paused)
{
    if (paused == relay_paused) {
        DBG("relay already paused for stream [%p], ignoring\n",
            static_cast<void *>(stream));
        return;
    }

    relay_paused = paused;
    DBG("relay %spaused, stream [%p]\n", relay_paused?"":"not ",
        static_cast<void *>(stream));

    if (nullptr != stream) {
        if (relay_paused)
            stream->disableRtpRelay();
        else
            stream->enableRtpRelay();
    }
}

void AudioStreamData::clearDtmfSink()
{
    if (dtmf_detector) {
        delete dtmf_detector;
        dtmf_detector = nullptr;
    }
    if (dtmf_queue) {
        delete dtmf_queue;
        dtmf_queue = nullptr;
    }
}

void AudioStreamData::setDtmfSink(AmDtmfSink *dtmf_sink)
{
    // TODO: optimize: clear & create the dtmf_detector only if the dtmf_sink changed
    clearDtmfSink();

    if (dtmf_sink && stream) {
        dtmf_detector = new AmDtmfDetector(dtmf_sink);
        dtmf_queue = new AmDtmfEventQueue(dtmf_detector);
        dtmf_detector->setInbandDetector(AmConfig.default_dtmf_detector, stream->getSampleRate());

        if(!enable_dtmf_transcoding && lowfi_payloads.size()) {
            string selected_payload_name = stream->getPayloadName(stream->getPayloadType());
            for(vector<SdpPayload>::iterator it = lowfi_payloads.begin();
                it != lowfi_payloads.end(); ++it)
            {
                DBG("checking %s/%i PL type against %s/%i\n",
                selected_payload_name.c_str(), stream->getPayloadType(),
                it->encoding_name.c_str(), it->payload_type);
                if(selected_payload_name == it->encoding_name) {
                    enable_dtmf_transcoding = true;
                    break;
                }
            }
        }
    }
}

bool AudioStreamData::initStream(PlayoutType playout_type,
    AmSdp &local_sdp, AmSdp &remote_sdp, int media_idx)
{
    resetStats();

    if (!stream) {
        initialized = false;
        return false;
    }

    // TODO: try to init only in case there are some payloads which can't be relayed
    stream->forceSdpMediaIndex(media_idx);

    stream->setOnHold(false); // just hack to do correctly mute detection in stream->init
    if (stream->init(local_sdp, remote_sdp, force_symmetric_rtp) == 0) {
        stream->setPlayoutType(playout_type);
        initialized = true;

        //do not unmute if muted because of 0.0.0.0 remote IP (the mute flag is set during init)
        //if (!stream->muted()) stream->setOnHold(muted);
    } else {
        initialized = false;
        DBG("stream initialization failed\n");
        // there still can be payloads to be relayed (if all possible payloads are
        // to be relayed this needs not to be an error)
    }
    stream->setOnHold(muted);
    stream->setReceiving(receiving);

    return initialized;
}

void AudioStreamData::sendDtmf(int event, unsigned int duration_ms)
{
  DBG("AudioStreamData::sendDtmf(event = %d, duration = %u)",event,duration_ms);
  if (stream) stream->sendDtmf(event,duration_ms);
}

void AudioStreamData::resetStats()
{
  if (outgoing_payload != UNDEFINED_PAYLOAD) {
    b2b_stats.decCodecWriteUsage(outgoing_payload_name);
    outgoing_payload = UNDEFINED_PAYLOAD;
    outgoing_payload_name.clear();
  }
  if (incoming_payload != UNDEFINED_PAYLOAD) {
    b2b_stats.decCodecReadUsage(incoming_payload_name);
    incoming_payload = UNDEFINED_PAYLOAD;
    incoming_payload_name.clear();
  }
}

void AudioStreamData::updateSendStats()
{
  if (!initialized) {
    resetStats();
    return;
  }

  int payload = stream->getPayloadType();
  if (payload != outgoing_payload) { 
    // payload used to send has changed

    // decrement usage of previous payload if set
    if (outgoing_payload != UNDEFINED_PAYLOAD) 
      b2b_stats.decCodecWriteUsage(outgoing_payload_name);
    
    if (payload != UNDEFINED_PAYLOAD) {
      // remember payload name (in lowercase to simulate case insensitivity)
      outgoing_payload_name = stream->getPayloadName(payload);
      transform(outgoing_payload_name.begin(), outgoing_payload_name.end(), 
          outgoing_payload_name.begin(), ::tolower);
      b2b_stats.incCodecWriteUsage(outgoing_payload_name);
    }
    else outgoing_payload_name.clear();
    outgoing_payload = payload;
  }
}

void AudioStreamData::updateRecvStats(AmRtpStream *s)
{
  if (!initialized) {
    resetStats();
    return;
  }

  int payload = s->getLastPayload();
  if (payload != incoming_payload) { 
    // payload used to send has changed

    // decrement usage of previous payload if set
    if (incoming_payload != UNDEFINED_PAYLOAD) 
      b2b_stats.decCodecReadUsage(incoming_payload_name);
    
    if (payload != UNDEFINED_PAYLOAD) {
      // remember payload name (in lowercase to simulate case insensitivity)
      incoming_payload_name = stream->getPayloadName(payload);
      transform(incoming_payload_name.begin(), incoming_payload_name.end(), 
          incoming_payload_name.begin(), ::tolower);
      b2b_stats.incCodecReadUsage(incoming_payload_name);
    }
    else incoming_payload_name.clear();
    incoming_payload = payload;
  }
}

int AudioStreamData::writeStream(unsigned long long ts, unsigned char *buffer, AudioStreamData &src)
{
    bool dtmf_detected = false;

    if (!initialized) {
        if(!in || !out) return 0;
        //non-stream mode
        if (!src.isInitialized()) return 0; //other leg MUST be initialized with stream
        AmRtpAudio *src_stream = src.getStream();
        if(src_stream->checkInterval(ts)) {
            int sample_rate = src_stream->getSampleRate();
            int got = src_stream->get(ts, buffer, sample_rate, src_stream->getFrameSize());
            //CLASS_DBG("src_stream->get(%llu,%d)",ts,got);
            if (got < 0) return -1;
            if (got > 0) {
                updateRecvStats(src_stream);
                //CLASS_DBG("out->put(%llu,%d)",ts,got);
                return out->put(ts, buffer, sample_rate,
                                static_cast<unsigned int>(got));
            }
        }
        return 0;
    }

    if (stream->getOnHold()) return 0; // ignore hold streams?

    unsigned int f_size = stream->getFrameSize();
    if (stream->sendIntReached(ts)) {
        // A leg is ready to send data
        int sample_rate = stream->getSampleRate();
        int got = 0;
        if (in) {
            //process src_stream even if custom input enabled
            if(src.isInitialized()) {
                AmRtpAudio *src_stream = src.getStream();
                if (src_stream->checkInterval(ts)||stream->getFrameTime() <= src_stream->getFrameTime()) {
                    int tmp_got = src_stream->get(ts, buffer, sample_rate, f_size);
                    //DBG("[%p] stream %p got %d from stream input %p",this,stream,got,src_stream);
                    if (tmp_got > 0) {
                        if(src_stream->isLastSamplesRelayed()) {
                            stream->record(ts, buffer, sample_rate,
                                           static_cast<unsigned int>(tmp_got));
                        } else {
                            updateRecvStats(src_stream);
                            if (dtmf_queue && enable_dtmf_transcoding) {
                                dtmf_queue->putDtmfAudio(dtmf_detected, buffer, tmp_got, ts);
                            }
                        }
                    }
                }
            }
            got = in->get(ts, buffer, sample_rate, f_size);
            //DBG("[%p] stream %p got %d from non-stream input %p",this,stream,got,in);
            if (got < 0) return 0;
        } else {
            if (!src.isInitialized()) {
                //non-stream mode
                AmAudio *src_in = src.getInput();
                if(!src_in) return 0;
                got = src_in->get(ts, buffer, sample_rate, f_size);
                //CLASS_DBG("src_in->get(%llu,%d)",ts,got);
            } else {
                AmRtpAudio *src_stream = src.getStream();
                if (src_stream->checkInterval(ts)|| stream->getFrameTime() <= src_stream->getFrameTime()) {
                    got = src_stream->get(ts, buffer, sample_rate, f_size);
                    //DBG("[%p] stream %p got %d from stream %p",this,stream,got,src_stream);
                    if (got > 0) {
                        src_stream->feedInbandDetector(buffer,static_cast<unsigned int>(got), ts);
                        if(src_stream->isLastSamplesRelayed()) {
                            stream->record(ts, buffer, sample_rate,
                                           static_cast<unsigned int>(got));
                            return 0;
                        } else {
                            updateRecvStats(src_stream);
                            if (dtmf_queue && enable_dtmf_transcoding) {
                                dtmf_queue->putDtmfAudio(dtmf_detected, buffer, got, ts);
                                if(enable_inbound_dtmf_filtering && dtmf_detected) {
                                    DBG("cut inbound dtmf from %p",static_cast<void *>(stream));
                                    memset(buffer,0,static_cast<unsigned int>(got));
                                    //got = src_stream->conceal_loss(PCM16_B2S(got),buffer);
                                }
                            }
                        }
                    }
                }
            }
            if (got < 0) return -1;
        }

        stream->processRtcpTimers(ts, stream->scaleSystemTS(ts));

        if (got > 0) {
            updateSendStats();
            //CLASS_DBG("stream->put(%llu,%d)",ts,got);
            return stream->put(ts, buffer, sample_rate,
                               static_cast<unsigned int>(got));
        }
    }
    return 0;
}

void AudioStreamData::mute(bool set_mute)
{
    DBG("mute(%s) - RTP stream [%p]\n", set_mute?"true":"false", static_cast<void *>(stream));

    if (stream) {
        stream->setOnHold(set_mute);
        if (muted != set_mute) stream->clearRTPTimeout();
    }
    muted = set_mute;
}

void AudioStreamData::setReceiving(bool r) {
    DBG("setReceiving(%s) - RTP stream [%p]\n", r?"true":"false", static_cast<void *>(stream));
    if (stream) {
        stream->setReceiving(r);
    }
    receiving = r;
}

//////////////////////////////////////////////////////////////////////////////////

AmB2BMedia::RelayStreamPair::RelayStreamPair(AmB2BSession *_a, AmB2BSession *_b)
  : a(_a, _a ? _a->getRtpInterface() : -1, _a ? _a->getRtpAddr() : -1),
    b(_b, _b ? _b->getRtpInterface() : -1, _a ? _a->getRtpAddr() : -1)
{ }

AmB2BMedia::AmB2BMedia(AmB2BSession *_a, AmB2BSession *_b): 
    a(_a), b(_b),
    callgroup(AmSession::getNewId()),
    have_a_leg_local_sdp(false), have_a_leg_remote_sdp(false),
    have_b_leg_local_sdp(false), have_b_leg_remote_sdp(false),
    ref_cnt(0), // everybody who wants to use must add one reference itselves
    playout_type(ADAPTIVE_PLAYOUT),
    a_leg_muted(false), b_leg_muted(false),
    relay_paused(false),
    logger(nullptr),
    asensor(nullptr), bsensor(nullptr),
    ignore_relay_streams(false)
{
    DBG("AmB2BMedia[%p](%p,%p) t",
        static_cast<void *>(this),
        static_cast<void *>(_a),
        static_cast<void *>(_b));
}

AmB2BMedia::~AmB2BMedia()
{
    DBG("~AmB2BMedia[%p]()",static_cast<void *>(this));
    if (logger) dec_ref(logger);
    if (asensor) dec_ref(asensor);
    if (bsensor) dec_ref(bsensor);
}

void AmB2BMedia::addToMediaProcessor()
{
    addReference(); // AmMediaProcessor's reference
    AmMediaProcessor::instance()->addSession(this, callgroup);
}

void AmB2BMedia::addToMediaProcessorUnsafe()
{
    ref_cnt++; // AmMediaProcessor's reference
    AmMediaProcessor::instance()->addSession(this, callgroup);
}

void AmB2BMedia::addReference() {
    mutex.lock();
    ref_cnt++;
    mutex.unlock();
}

bool AmB2BMedia::releaseReference() {
    mutex.lock();
    int r = --ref_cnt;
    mutex.unlock();
    if (r==0) {
        DBG("last reference to AmB2BMedia [%p] cleared, destroying\n",
            static_cast<void *>(this));
        delete this;
    }
    return (r == 0);
}

void AmB2BMedia::changeSession(bool a_leg, AmB2BSession *new_session)
{
    AmLock lock(mutex);
    changeSessionUnsafe(a_leg, new_session);
}

void AmB2BMedia::changeSessionUnsafe(bool a_leg, AmB2BSession *new_session)
{
    TRACE("changing %s leg session to %p\n", a_leg ? "A" : "B",
          static_cast<void *>(new_session));

    if (a_leg) {
        if(a) a->onSessionChange(new_session);
        a = new_session;
    } else {
        if(b) b->onSessionChange(new_session);
        b = new_session;
    }

    bool needs_processing = a && b && a->getRtpRelayMode() == AmB2BSession::RTP_Transcoding;

    // update all streams
    for (AudioStreamIterator i = audio.begin(); i != audio.end(); ++i) {
        // stop processing first to avoid unexpected results
        i->a.stopStreamProcessing();
        i->b.stopStreamProcessing();

        // replace session
        if (a_leg) {
            i->a.changeSession(new_session);
        } else {
            i->b.changeSession(new_session);
        }

        updateStreamPair(*i);

        if (i->requiresProcessing()) needs_processing = true;

        // reset logger (needed if a stream changes)
        i->setLogger(logger);
        i->setASensor(asensor);
        i->setBSensor(bsensor);

        // return back for processing if needed
        i->a.resumeStreamProcessing();
        i->b.resumeStreamProcessing();
    }

    for (RelayStreamIterator j = relay_streams.begin(); j != relay_streams.end(); ++j) {
        AmRtpStream &a = (*j)->a;
        AmRtpStream &b = (*j)->b;
        // FIXME: is stop & resume receiving needed here?
        if(a_leg) a.changeSession(new_session);
        else b.changeSession(new_session);
    }

    if (needs_processing) {
        if (!isProcessingMedia()) {
            addToMediaProcessorUnsafe();
        }
    } else if (isProcessingMedia()) {
        AmMediaProcessor::instance()->removeSession(this);
    }

    TRACE("session changed\n");
}

int AmB2BMedia::writeStreams(unsigned long long ts, unsigned char *buffer)
{
    int res = 0;
    AmLock lock(mutex);
    for (AudioStreamIterator i = audio.begin(); i != audio.end(); ++i) {
        if (i->a.writeStream(ts, buffer, i->b) < 0) { res = -1; break; }
        if (i->b.writeStream(ts, buffer, i->a) < 0) { res = -1; break; }
    }
    return res;
}

void AmB2BMedia::processDtmfEvents()
{
  AmLock lock(mutex);
  for (AudioStreamIterator i = audio.begin(); i != audio.end(); ++i) {
    i->a.processDtmfEvents();
    i->b.processDtmfEvents();
  }

  if (a) a->processDtmfEvents();
  if (b) b->processDtmfEvents();
}

void AmB2BMedia::sendDtmf(bool a_leg, int event, unsigned int duration_ms)
{
    AmLock lock(mutex);
    if(!audio.size())
        return;

    // send the DTMFs using the first available stream
    if(a_leg) audio[0].a.sendDtmf(event,duration_ms);
    else audio[0].b.sendDtmf(event,duration_ms);
}

void AmB2BMedia::clearAudio(bool a_leg)
{
    TRACE("[%p] clear %s leg audio\n",
          static_cast<void *>(this), a_leg ? "A" : "B");

    AmLock lock(mutex);

    for (AudioStreamIterator i = audio.begin(); i != audio.end(); ++i) {
        // remove streams from AmRtpReceiver first! (always both?)
        i->a.stopStreamProcessing();
        i->b.stopStreamProcessing();
        if (a_leg) {
            i->a.clear();
            i->b.setRelayStream(nullptr);
        } else {
            i->b.clear();
            i->a.setRelayStream(nullptr);
        }
    }

    for (RelayStreamIterator j = relay_streams.begin(); j != relay_streams.end(); ++j) {
        (*j)->a.stopReceiving();
        (*j)->b.stopReceiving();
    }

    // forget sessions to avoid using them once clearAudio is called
    changeSessionUnsafe(a_leg, nullptr);

    if(a_leg) {
        have_a_leg_local_sdp = false;
        have_a_leg_remote_sdp = false;
    } else {
        have_b_leg_local_sdp = false;
        have_b_leg_remote_sdp = false;
    }

    if (!a && !b) {
        audio.clear(); // both legs cleared
        for (RelayStreamIterator j = relay_streams.begin(); j != relay_streams.end(); ++j) {
            delete *j;
        }
        relay_streams.clear();
    }
}

void AmB2BMedia::clearRTPTimeout()
{
    AmLock lock(mutex);

    for (AudioStreamIterator i = audio.begin(); i != audio.end(); ++i) {
        i->a.clearRTPTimeout();
        i->b.clearRTPTimeout();
    }
}

bool AmB2BMedia::canRelay(const SdpMedia &m)
{
    return (m.transport == TP_RTPAVP) ||
           (m.transport == TP_RTPSAVP) ||
           (m.transport == TP_UDP) ||
           (m.transport == TP_UDPTL);
}

void AmB2BMedia::createStreams(const AmSdp &sdp)
{
    AudioStreamIterator astreams = audio.begin();
    RelayStreamIterator rstreams = relay_streams.begin();
    vector<SdpMedia>::const_iterator m = sdp.media.begin();
    int idx = 0;
    bool create_audio = astreams == audio.end();
    bool create_relay = rstreams == relay_streams.end();

    for (; m != sdp.media.end(); ++m, ++idx) {

        // audio streams
        if (m->type == MT_AUDIO) {
            DBG("createStreams() processing audio stream %d",idx);
            if (create_audio) {
                AudioStreamPair pair(a, b, idx);
                DBG("[%p] createStreams() created AudioStreamPair for stream %d. %p/%p",
                    static_cast<void *>(this),
                    idx,
                    static_cast<void *>(pair.a.getStream()),
                    static_cast<void *>(pair.b.getStream()));
                pair.a.mute(a_leg_muted);
                pair.b.mute(b_leg_muted);
                audio.push_back(pair);
                audio.back().setLogger(logger);
                audio.back().setASensor(asensor);
                audio.back().setBSensor(bsensor);
            } else if (++astreams == audio.end()) {
                create_audio = true; // we went through the last audio stream
            }
        } else if(!ignore_relay_streams && canRelay(*m)) {// non-audio streams that we can relay
            DBG("createStreams() processing non-audio stream %d",idx);
            if (create_relay) {
                relay_streams.push_back(new RelayStreamPair(a, b));
                RelayStreamPair*& streams_pair = relay_streams.back();
                DBG("[%p] createStreams() created RelayStreamPair for non-audio stream %d. %p/%p",
                    static_cast<void *>(this),
                    idx,
                    static_cast<void *>(&streams_pair->a),
                    static_cast<void *>(&streams_pair->b));
                streams_pair->setLogger(logger);
                streams_pair->setASensor(asensor);
                streams_pair->setBSensor(bsensor);
            } else if (++rstreams == relay_streams.end()) {
                create_relay = true; // we went through the last relay stream
            }
        }
    } //for (; m != sdp.media.end(); ++m, ++idx)
}

void AmB2BMedia::replaceConnectionAddress(
    AmSdp &parser_sdp, bool a_leg,
    const string& relay_address,
    const string& relay_public_address,
    int addr_type)
{
    AmLock lock(mutex);

    SdpConnection orig_conn = parser_sdp.conn; // needed for the 'quick workaround' for non-audio media

    // place relay_address in connection address
    if (!parser_sdp.conn.address.empty() &&
        (parser_sdp.conn.address != zero_ip))
    {
        parser_sdp.conn.address = relay_public_address;
        DBG("new connection address: %s",parser_sdp.conn.address.c_str());
    }

    // we need to create streams if they are not already created
    createStreams(parser_sdp);

    string replaced_ports;

    AudioStreamIterator audio_stream_it = audio.begin();
    RelayStreamIterator relay_stream_it = relay_streams.begin();

    std::vector<SdpMedia>::iterator it = parser_sdp.media.begin();
    for (; it != parser_sdp.media.end() ; ++it) {
        // FIXME: only UDP streams are handled for now
        if(it->type == MT_AUDIO) {
            if( audio_stream_it == audio.end() ) {
                // strange... we should actually have a stream for this media line...
                DBG("audio media line does not have coresponding audio stream...\n");
                continue;
            }
            if(it->port) { // if stream active
                if (!it->conn.address.empty() && (parser_sdp.conn.address != zero_ip)) {
                    it->conn.address = relay_public_address;
                    it->conn.addrType = addr_type;
                    DBG("new stream connection address: %s",it->conn.address.c_str());
                }
                try {
                    if (a_leg) {
                        audio_stream_it->a.replaceAudioMediaParameters(*it, relay_address);
                    } else {
                        audio_stream_it->b.replaceAudioMediaParameters(*it, relay_address);
                    }
                    if(!replaced_ports.empty()) replaced_ports += "/";
                    replaced_ports += int2str(it->port);
                } catch (const string& s) {
                    ERROR("setting port: '%s'\n", s.c_str());
                    throw string("error setting RTP port\n");
                }
            }
            ++audio_stream_it;
        } else if(canRelay(*it)) {
            if( relay_stream_it == relay_streams.end() ) {
                // strange... we should actually have a stream for this media line...
                DBG("media line does not have a coresponding relay stream...\n");
                continue;
            }
            if(it->port) { // if stream active
                if (!it->conn.address.empty() && (parser_sdp.conn.address != zero_ip)) {
                    it->conn.address = relay_public_address;
                    it->conn.addrType = addr_type;
                    DBG("new stream connection address: %s",it->conn.address.c_str());
                }
                try {
                    if (a_leg) {
                        if(!(*relay_stream_it)->a.hasLocalSocket()) {
                            (*relay_stream_it)->a.setLocalIP(relay_address);
                        }
                        it->port = static_cast<unsigned int>((*relay_stream_it)->a.getLocalPort());
                        replaceRtcpAttr(*it, relay_address, (*relay_stream_it)->a.getLocalRtcpPort());
                    } else {
                        if(!(*relay_stream_it)->b.hasLocalSocket()) {
                            (*relay_stream_it)->b.setLocalIP(relay_address);
                        }
                        it->port = static_cast<unsigned int>((*relay_stream_it)->b.getLocalPort());
                        replaceRtcpAttr(*it, relay_address, (*relay_stream_it)->b.getLocalRtcpPort());
                    }
                    if(!replaced_ports.empty()) replaced_ports += "/";
                    replaced_ports += int2str(it->port);
                } catch (const string& s) {
                    ERROR("setting port: '%s'\n", s.c_str());
                    throw string("error setting RTP port\n");
                }
            }
            ++relay_stream_it;
        } else {
            // quick workaround to allow direct connection of non-supported streams (i.e.
            // those which are not relayed or transcoded): propagate connection
            // address - might work but need not (to be tested with real clients
            // instead of simulators)
            if (it->conn.address.empty()) it->conn = orig_conn;
            continue;
        }
    } //for (; it != parser_sdp.media.end() ; ++it) {

    if (it != parser_sdp.media.end()) {
        // FIXME: create new streams here?
        WARN("trying to relay SDP with more media lines than "
             "relay streams initialized (%zu)",
             audio.size()+relay_streams.size());
    }

    DBG("replaced connection address in SDP with %s:%s",
        relay_public_address.c_str(), replaced_ports.c_str());
}

void AmB2BMedia::updateStreamPair(AudioStreamPair &pair)
{
    bool have_a = have_a_leg_local_sdp && have_a_leg_remote_sdp;
    bool have_b = have_b_leg_local_sdp && have_b_leg_remote_sdp;

    TRACE("updating stream in A leg");
    if (have_a) pair.a.initStream(playout_type, a_leg_local_sdp, a_leg_remote_sdp, pair.media_idx);
    pair.a.setDtmfSink(b);

    TRACE("updating stream in B leg");
    pair.b.setDtmfSink(a);
    if (have_b) pair.b.initStream(playout_type, b_leg_local_sdp, b_leg_remote_sdp, pair.media_idx);

    TRACE("update relay for stream in A leg");
    if (pair.b.getInput()) pair.a.setRelayStream(nullptr); // don't mix relayed RTP into the other's input
    else pair.a.setRelayStream(pair.b.getStream());

    TRACE("update relay for stream in B leg");
    if (pair.a.getInput()) pair.b.setRelayStream(nullptr); // don't mix relayed RTP into the other's input
    else pair.b.setRelayStream(pair.a.getStream());

    TRACE("[%p] audio streams %p/%p updated\n",
          static_cast<void *>(this),
          static_cast<void *>(pair.a.getStream()),
          static_cast<void *>(pair.b.getStream()));
}

void AmB2BMedia::updateAudioStreams()
{
    // SDP was updated
    TRACE("handling SDP change, A leg: %c%c, B leg: %c%c\n",
          have_a_leg_local_sdp ? 'X' : '-',
          have_a_leg_remote_sdp ? 'X' : '-',
          have_b_leg_local_sdp ? 'X' : '-',
          have_b_leg_remote_sdp ? 'X' : '-');

    // if we have all necessary information we can initialize streams and start
    // their processing
    if (audio.empty() && relay_streams.empty()) return; // no streams

    bool have_a = have_a_leg_local_sdp && have_a_leg_remote_sdp;
    bool have_b = have_b_leg_local_sdp && have_b_leg_remote_sdp;

    if(!((have_a || have_b))) return;

    bool needs_processing = a && b && a->getRtpRelayMode() == AmB2BSession::RTP_Transcoding;

    // initialize streams to be able to relay & transcode (or use local audio)
    for (AudioStreamIterator i = audio.begin(); i != audio.end(); ++i) {
        i->a.stopStreamProcessing();
        i->b.stopStreamProcessing();

        updateStreamPair(*i);

        if (i->requiresProcessing()) needs_processing = true;

        i->a.resumeStreamProcessing();
        i->b.resumeStreamProcessing();
    }

    // start media processing (only if transcoding or regular audio processing
    // required)
    // Note: once we send local SDP to the other party we have to expect RTP but
    // we need to be fully initialised (both legs) before we can correctly handle
    // the media, right?
    if (needs_processing) {
        if (!isProcessingMedia()) {
            addToMediaProcessorUnsafe();
        }
    } else if (isProcessingMedia()) {
        AmMediaProcessor::instance()->removeSession(this);
    }
}

void AmB2BMedia::updateRelayStream(
    AmRtpStream *stream, AmB2BSession *session,
    const string& connection_address,
    const SdpMedia &m, AmRtpStream *relay_to)
{
    static const PayloadMask true_mask(true);

    stream->stopReceiving();
    if(m.port) {
        if (session) {
            // propagate session settings
            stream->setPassiveMode(session->getRtpRelayForceSymmetricRtp());
            stream->setRtpRelayTransparentSeqno(session->getRtpRelayTransparentSeqno());
            stream->setRtpRelayTransparentSSRC(session->getRtpRelayTransparentSSRC());
            // if (!stream->hasLocalSocket()) stream->setLocalIP(session->advertisedIP());
        }
        stream->setRelayStream(relay_to);
        stream->setRelayPayloads(true_mask);
        if (!relay_paused) stream->enableRtpRelay();
        stream->setRAddr(connection_address,
                         static_cast<unsigned short>(m.port),
                         static_cast<unsigned short>(m.port+1));
        if((m.transport != TP_RTPAVP) && (m.transport != TP_RTPSAVP)) stream->enableRawRelay();
        stream->resumeReceiving();
    } else {
        DBG("disabled stream");
    }
}

void AmB2BMedia::createUpdateStreams(
    bool a_leg,
    const AmSdp &local_sdp, const AmSdp &remote_sdp,
    RelayController *ctrl)
{
    TRACE("%s (%c): create/updating streams with local & remote SDP\n",
          a_leg ? (a ? a->getLocalTag().c_str() : NULL) : (b ? b->getLocalTag().c_str() : NULL),
          a_leg ? 'A': 'B');

    /*string s;
    local_sdp.print(s);
    INFO("local SDP: %s\n", s.c_str());
    remote_sdp.print(s);
    INFO("remote SDP: %s\n", s.c_str());*/

    AmLock lock(mutex);

    if (a_leg) {
        a_leg_local_sdp = local_sdp;
        a_leg_remote_sdp = remote_sdp;
        have_a_leg_local_sdp = true;
        have_a_leg_remote_sdp = true;
    } else {
        b_leg_local_sdp = local_sdp;
        b_leg_remote_sdp = remote_sdp;
        have_b_leg_local_sdp = true;
        have_b_leg_remote_sdp = true;
    }

    // streams should be created already (replaceConnectionAddress called
    // before updateLocalSdp uses/assignes their port numbers)
    // create missing streams
    createStreams(local_sdp); // FIXME: remote_sdp?

    updateStreams(a_leg, ctrl);
}

void AmB2BMedia::updateStreams(bool a_leg, RelayController *ctrl)
{
    //const AmSdp &local_sdp = a_leg ? a_leg_local_sdp : b_leg_local_sdp;
    const AmSdp &remote_sdp = a_leg ? a_leg_remote_sdp : b_leg_remote_sdp;

    // compute relay mask for every stream
    // Warning: do not apply the new mask unless the offer answer succeeds?
    // we can safely apply the changes once we have local & remote SDP (i.e. the
    // negotiation is finished) otherwise we might handle the RTP in a wrong way

    AudioStreamIterator astream = audio.begin();
    RelayStreamIterator rstream = relay_streams.begin();
    int idx = 0;
    for (vector<SdpMedia>::const_iterator m = remote_sdp.media.begin();
         m != remote_sdp.media.end(); ++m, ++idx)
    {
        const string& connection_address = (m->conn.address.empty() ? remote_sdp.conn.address : m->conn.address);
        if (m->type == MT_AUDIO) {
            DBG("updateStreams() processing audio stream %d",idx);
            if(astream == audio.end()) {
                WARN("can't process audio stream from sdp. no appropriate audio stream. ignore it");
                continue;
            }
            DBG("[%p] updateStreams() update AudioStreamPair %p/%p",
                static_cast<void *>(this),
                static_cast<void *>(astream->a.getStream()),
                static_cast<void *>(astream->b.getStream()));

            // initialize relay mask in the other(!) leg and relay destination for stream in current leg
            TRACE("relay payloads in direction %s\n", a_leg ? "B -> A" : "A -> B");

            if (a_leg) {
                astream->b.setRelayPayloads(*m, ctrl);
                astream->a.setRelayDestination(connection_address,
                                               static_cast<int>(m->port));
            } else {
                astream->a.setRelayPayloads(*m, ctrl);
                astream->b.setRelayDestination(connection_address,
                                               static_cast<int>(m->port));
            }

            ++astream;
        } else {
            DBG("updateStreams() processing non-audio stream %d",idx);
            if(ignore_relay_streams) continue;
            if (!canRelay(*m)) continue;
            if (rstream == relay_streams.end()) continue;

            RelayStreamPair& relay_stream = **rstream;

            if(a_leg) {
                DBG("[%p] updating A-leg relay_stream %d. %p",
                    static_cast<void *>(this),
                    idx,
                    static_cast<void *>(&relay_stream.a));
                updateRelayStream(&relay_stream.a, a, connection_address, *m, &relay_stream.b);
            } else {
                DBG("[%p] updating B-leg relay_stream %d. %p",
                    static_cast<void *>(this),
                    idx,
                    static_cast<void *>(&relay_stream.b));
                updateRelayStream(&relay_stream.b, b, connection_address, *m, &relay_stream.a);
            }

            ++rstream;
        }
    } //iterate remote_sdp.media

    updateAudioStreams();

    TRACE("streams updated with SDP");
}

void AmB2BMedia::setFirstAudioPairStream(
    bool a_leg,
    AmRtpAudio *stream,
    const AmSdp &local_sdp, const AmSdp &remote_sdp)
{
    if(audio.empty()) {
        AudioStreamPair pair(nullptr, nullptr, 0);
        audio.push_back(pair);
    }

    AudioStreamPair &pair = *audio.begin();
    AudioStreamData &adata = a_leg ? pair.a : pair.b;
    adata.setStreamUnsafe(stream);

    // save SDP: FIXME: really needed to store instead of just to use?
    if (a_leg) {
        a_leg_local_sdp = local_sdp;
        a_leg_remote_sdp = remote_sdp;
        have_a_leg_local_sdp = true;
        have_a_leg_remote_sdp = true;
    } else {
        b_leg_local_sdp = local_sdp;
        b_leg_remote_sdp = remote_sdp;
        have_b_leg_local_sdp = true;
        have_b_leg_remote_sdp = true;
    }
}

void AmB2BMedia::stop(bool a_leg)
{
    TRACE("stop %s leg\n", a_leg ? "A" : "B");
    clearAudio(a_leg);
    // remove from processor only if both A and B leg stopped
    if (isProcessingMedia() && (!a) && (!b)) {
        AmMediaProcessor::instance()->removeSession(this);
    }
}

void AmB2BMedia::onMediaProcessingTerminated()
{
    AmMediaSession::onMediaProcessingTerminated();

    // release reference held by AmMediaProcessor
    releaseReference();
}

bool AmB2BMedia::replaceOffer(AmSdp &sdp, bool a_leg)
{
    TRACE("replacing offer with a local one\n");
    AmLock lock(mutex);

    createStreams(sdp); // create missing streams

    try {

        AudioStreamIterator as = audio.begin();
        for (vector<SdpMedia>::iterator m = sdp.media.begin(); m != sdp.media.end(); ++m) {
            if (m->type == MT_AUDIO && as != audio.end()) {
                // generate our local offer
                TRACE("... making audio stream offer\n");
                if (a_leg) as->a.getSdpOffer(as->media_idx, *m);
                else as->b.getSdpOffer(as->media_idx, *m);
                ++as;
            } else {
                TRACE("... making non-audio/uninitialised stream inactive\n");
                m->send = false;
                m->recv = false;
            }
        }
    } catch (...) {
        TRACE("hold SDP offer creation failed\n");
        return true;
    }

    TRACE("hold SDP offer generated\n");
    return true;
}

bool AmB2BMedia::haveLocalSdp(bool a_leg)
{
    if(a_leg) return have_a_leg_local_sdp;
    else return have_b_leg_local_sdp;
}

bool AmB2BMedia::haveRemoteSdp(bool a_leg)
{
    if(a_leg) return have_a_leg_remote_sdp;
    else return have_b_leg_remote_sdp;
}

const AmSdp &AmB2BMedia::getLocalSdp(bool a_leg)
{
    if(a_leg) return a_leg_local_sdp;
    else return b_leg_local_sdp;
}

const AmSdp &AmB2BMedia::getRemoteSdp(bool a_leg)
{
    if(a_leg) return a_leg_remote_sdp;
    else return b_leg_remote_sdp;
}

void AmB2BMedia::setMuteFlag(bool a_leg, bool set)
{
    AmLock lock(mutex);
    if (a_leg) a_leg_muted = set;
    else b_leg_muted = set;
    for (AudioStreamIterator i = audio.begin(); i != audio.end(); ++i) {
        if (a_leg) i->a.mute(set);
        else i->b.mute(set);
    }
}

void AmB2BMedia::setRtpTimeout(bool a_leg, unsigned int timeout)
{
    AmLock lock(mutex);
    for (AudioStreamPair &p: audio)
        p.setRtpTimeout(a_leg,timeout);
    for (RelayStreamPair * &p: relay_streams)
        p->setRtpTimeout(a_leg,timeout);
}

void AmB2BMedia::setRtpTimeout(unsigned int timeout)
{
    AmLock lock(mutex);
    for (AudioStreamPair &p: audio)
        p.setRtpTimeout(timeout);
    for (RelayStreamPair * &p: relay_streams)
        p->setRtpTimeout(timeout);
}

void AmB2BMedia::setFirstStreamInput(bool a_leg, AmAudio *in)
{
    AmLock lock(mutex);
    if (!audio.empty()) {
        AudioStreamIterator i = audio.begin();
        if (a_leg) i->a.setInput(in);
        else i->b.setInput(in);
        updateAudioStreams();
    } else {
        if (in) {
            ERROR("BUG: can't set %s leg's first stream input, no streams\n", a_leg ? "A": "B");
        }
    }
}

void AmB2BMedia::setFirstStreamOutput(bool a_leg, AmAudio *out)
{
    AmLock lock(mutex);
    if (!audio.empty()) {
        AudioStreamIterator i = audio.begin();
        if (a_leg) i->a.setOutput(out);
        else i->b.setOutput(out);
        updateAudioStreams();
    } else {
        if (out) {
            ERROR("BUG: can't set %s leg's first stream output, no streams\n", a_leg ? "A": "B");
        }
    }
}

void AmB2BMedia::createHoldAnswer(bool a_leg, const AmSdp &offer, AmSdp &answer, bool use_zero_con)
{
    // because of possible RTP relaying our payloads need not to match the remote
    // party's payloads (i.e. we might need not understand the remote party's
    // codecs)
    // As a quick hack we may use just copy of the original SDP with all streams
    // deactivated to avoid sending RTP to us (twinkle requires at least one
    // non-disabled stream in the response so we can not set all ports to 0 to
    // signalize that we don't want to receive anything)

    AmLock lock(mutex);

    answer = offer;
    answer.media.clear();

    if (use_zero_con) {
        answer.conn.address = zero_ip;
    } else {
        if (a_leg) {
            if (a) answer.conn.address = a->advertisedIP();
        } else {
            if (b) answer.conn.address = b->advertisedIP();
        }
        if (answer.conn.address.empty())
            answer.conn.address = zero_ip; // we need something there
    }

    AudioStreamIterator i = audio.begin();
    vector<SdpMedia>::const_iterator m;
    for (m = offer.media.begin();
         m != offer.media.end(); ++m)
    {
        answer.media.push_back(SdpMedia());
        SdpMedia &media = answer.media.back();
        media.type = m->type;

        if (media.type != MT_AUDIO) { // copy whole media line except port
            media = *m ;
            media.port = 0;
            continue;
        }

        if (m->port == 0) { // copy whole inactive media line
            media = *m;
            ++i;
            continue;
        }

        if (a_leg) i->a.getSdpAnswer(i->media_idx, *m, media);
        else i->b.getSdpAnswer(i->media_idx, *m, media);

        media.send = false; // should be already because the stream should be on hold
        media.recv = false; // what we would do with received data?

        if (media.payloads.empty()) {
            // we have to add something there
            if (!m->payloads.empty())
                media.payloads.push_back(m->payloads[0]);
        }
        break;
    }
}

void AmB2BMedia::setRtpLogger(msg_logger* _logger)
{
    AmLock lock(mutex);

    if (logger) dec_ref(logger);
    logger = _logger;
    if (logger) inc_ref(logger);

    // walk through all the streams and use logger for them
    for (AudioStreamIterator i = audio.begin(); i != audio.end(); ++i)
        i->setLogger(logger);
    for (RelayStreamIterator j = relay_streams.begin(); j != relay_streams.end(); ++j)
        (*j)->setLogger(logger);
}

void AmB2BMedia::setRtpASensor(msg_sensor* _sensor)
{
    DBG("AmB2BMedia: change B sensors to %p",
        static_cast<void *>(_sensor));

    AmLock lock(mutex);
    if(asensor) dec_ref(asensor);
    asensor = _sensor;
    if(asensor) inc_ref(asensor);

    // walk through all the streams and apply sensor for them
    for (AudioStreamIterator i = audio.begin(); i != audio.end(); ++i)
        i->setASensor(asensor);
    for (RelayStreamIterator j = relay_streams.begin(); j != relay_streams.end(); ++j)
        (*j)->setASensor(asensor);
}

void AmB2BMedia::setRtpBSensor(msg_sensor* _sensor)
{
    DBG("AmB2BMedia: change B sensors to %p",
        static_cast<void *>(_sensor));

    AmLock lock(mutex);
    if(bsensor) dec_ref(bsensor);
    bsensor = _sensor;
    if(bsensor) inc_ref(bsensor);

    // walk through all the streams and apply sensor for them
    for (AudioStreamIterator i = audio.begin(); i != audio.end(); ++i)
        i->setBSensor(bsensor);
    for (RelayStreamIterator j = relay_streams.begin(); j != relay_streams.end(); ++j)
        (*j)->setBSensor(bsensor);
}

void AmB2BMedia::setRelayDTMFReceiving(bool enabled) {
    AmLock lock(mutex);

    DBG("relay_streams.size() = %zd, audio_streams.size() = %zd\n", relay_streams.size(), audio.size());
    for (RelayStreamIterator j = relay_streams.begin(); j != relay_streams.end(); j++) {
        DBG("force_receive_dtmf %sabled for [%p]\n", enabled?"en":"dis",
            static_cast<void *>(&(*j)->a));
        DBG("force_receive_dtmf %sabled for [%p]\n", enabled?"en":"dis",
            static_cast<void *>(&(*j)->b));
        (*j)->a.force_receive_dtmf = enabled;
        (*j)->b.force_receive_dtmf = enabled;
    }

    for (AudioStreamIterator j = audio.begin(); j != audio.end(); j++) {
        DBG("force_receive_dtmf %sabled for [%p]\n", enabled?"en":"dis",
            static_cast<void *>(j->a.getStream()));
        DBG("force_receive_dtmf %sabled for [%p]\n", enabled?"en":"dis",
            static_cast<void *>(j->b.getStream()));
        if(nullptr != j->a.getStream())
            j->a.getStream()->force_receive_dtmf = enabled;
        if(nullptr != j->b.getStream())
            j->b.getStream()->force_receive_dtmf = enabled;
    }
}

/** set receving of RTP/relay streams (not receiving=drop incoming packets) */
void AmB2BMedia::setReceiving(bool receiving_a, bool receiving_b)
{
    AmLock lock(mutex); // TODO: is this necessary?

    DBG("relay_streams.size() = %zd, audio_streams.size() = %zd\n",
        relay_streams.size(), audio.size());

    for (RelayStreamIterator j = relay_streams.begin(); j != relay_streams.end(); j++)
    {
        DBG("setReceiving(%s) A relay stream [%p]\n", receiving_a?"true":"false",
            static_cast<void *>(&(*j)->a));
        (*j)->a.setReceiving(receiving_a);
        DBG("setReceiving(%s) B relay stream [%p]\n", receiving_b?"true":"false",
            static_cast<void *>(&(*j)->b));
        (*j)->b.setReceiving(receiving_b);
    }

    for (AudioStreamIterator j = audio.begin(); j != audio.end(); j++) {
        DBG("setReceiving(%s) A audio stream [%p]\n", receiving_a?"true":"false",
            static_cast<void *>(j->a.getStream()));
        j->a.setReceiving(receiving_a);
        DBG("setReceiving(%s) B audio stream [%p]\n", receiving_b?"true":"false",
            static_cast<void *>(j->b.getStream()));
        j->b.setReceiving(receiving_b);
    }
}

void AmB2BMedia::setIgnoreRelayStreams(bool ignore)
{
    ignore_relay_streams = ignore;
    DBG("relay streams ignore %sabled",ignore?"en":"dis");
}

void AmB2BMedia::pauseRelay()
{
    AmLock lock(mutex);

    DBG("relay_streams.size() = %zd, audio_streams.size() = %zd\n", relay_streams.size(), audio.size());
    relay_paused = true;
    for (RelayStreamIterator j = relay_streams.begin(); j != relay_streams.end(); j++) {
        (*j)->a.disableRawRelay();
        (*j)->b.disableRawRelay();
    }

    for (AudioStreamIterator j = audio.begin(); j != audio.end(); j++) {
        j->a.setRelayPaused(true);
        j->b.setRelayPaused(true);
    }
}

void AmB2BMedia::restartRelay()
{
    AmLock lock(mutex);

    DBG("relay_streams.size() = %zd, audio_streams.size() = %zd\n",
        relay_streams.size(), audio.size());

    relay_paused = false;

    for (RelayStreamIterator j = relay_streams.begin(); j != relay_streams.end(); j++) {
        (*j)->a.enableRawRelay();
        (*j)->b.enableRawRelay();
    }

    for (AudioStreamIterator j = audio.begin(); j != audio.end(); j++) {
        j->a.setRelayPaused(false);
        j->b.setRelayPaused(false);
    }
}

void AudioStreamData::setStreamUnsafe(AmRtpAudio *s)
{
    stream = s;
    shared_stream = true;
    force_symmetric_rtp = stream->getPassiveMode();
    initialized = true;
}

void AudioStreamData::debug()
{
    DBG("\tmuted: %s\n", muted ? "yes" : "no");
    if(stream) {
        stream->debug();
    } else {
        DBG("\t<null> <-> <null>");
    }
}

void AudioStreamData::getInfo(AmArg &ret)
{
    ret["muted"] = muted;
    ret["outgoing_payload"] = outgoing_payload_name;
    ret["incoming_payload"] = incoming_payload_name;

    AmArg &a = ret["stream"];
    stream->getInfo(a);
}

// print debug info
void AmB2BMedia::debug()
{
    AmLock lock(mutex);
    // walk through all the streams
    DBG("B2B media session %p ('%s' <-> '%s'):",
        static_cast<void *>(this),
        a ? a->getLocalTag().c_str() : "?",
        b ? b->getLocalTag().c_str() : "?");

    DBG("\tOA status: %c%c / %c%c",
        have_a_leg_local_sdp ? 'X' : '-',
        have_a_leg_remote_sdp ? 'X' : '-',
        have_b_leg_local_sdp ? 'X' : '-',
        have_b_leg_remote_sdp ? 'X' : '-');

    for (AudioStreamIterator i = audio.begin(); i != audio.end(); ++i) {
        DBG(" - audio stream (A):\n");
        i->a.debug();
        DBG(" - audio stream (B):\n");
        i->b.debug();
    }

    for (RelayStreamIterator j = relay_streams.begin();
         j != relay_streams.end(); ++j)
    {
        DBG(" - relay stream (A):\n");
        (*j)->a.debug();
        DBG(" - relay stream (B):\n");
        (*j)->b.debug();
    }
}

void AmB2BMedia::getInfo(AmArg &ret)
{
    ret["a_tag"] = a ? a->getLocalTag() : "nullptr";
    ret["b_tag"] = b ? b->getLocalTag() : "nullptr";

    AmArg &arg_audio = ret["audio_streams"];
    arg_audio.assertArray();
    for (AudioStreamIterator i = audio.begin(); i != audio.end(); ++i) {
        arg_audio.push(AmArg());
        AmArg &u = arg_audio.back();
        u["media_idx"] = i->media_idx;
        i->a.getInfo(u["a"]);
        i->b.getInfo(u["b"]);
    }

    AmArg &arg_relay_streams = ret["relay_streams"];
    arg_relay_streams.assertArray();
    for (RelayStreamIterator j = relay_streams.begin(); j != relay_streams.end(); ++j) {
        arg_relay_streams.push(AmArg());
        AmArg &u = arg_relay_streams.back();
        (*j)->a.getInfo(u["a"]);
        (*j)->b.getInfo(u["b"]);
    }

#define add_sdp_info(var)\
    if(have_##var){\
        AmArg &a = ret[#var];\
        var.getInfo(a);\
    } else {\
        ret[#var] = "empty";\
    }

    add_sdp_info(a_leg_local_sdp);
    add_sdp_info(a_leg_remote_sdp);
    add_sdp_info(b_leg_local_sdp);
    add_sdp_info(b_leg_remote_sdp);

#undef add_sdp_info
}
