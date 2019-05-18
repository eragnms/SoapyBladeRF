/*
 * This file is part of the bladeRF project:
 *   http://www.github.com/nuand/bladeRF
 *
 * Copyright (C) 2015-2018 Josh Blum
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "bladeRF_SoapySDR.hpp"
#include <SoapySDR/Logger.hpp>
#include <stdexcept>
#include <iostream>

//cross platform usleep()
#ifdef _MSC_VER
#include <windows.h>
#define usleep(t) Sleep((t)/1000)
#else
#include <unistd.h>
#endif

#define DEF_NUM_BUFFS 32
#define DEF_BUFF_LEN 4096

std::vector<std::string> bladeRF_SoapySDR::getStreamFormats(const int, const size_t) const
{
        std::vector<std::string> formats;
        formats.push_back("CS16");
        formats.push_back("CF32");
        return formats;
}

std::string bladeRF_SoapySDR::getNativeStreamFormat(const int, const size_t, double &fullScale) const
{
        fullScale = 2048;
        return "CS16";
}

SoapySDR::ArgInfoList bladeRF_SoapySDR::getStreamArgsInfo(const int, const size_t) const
{
        SoapySDR::ArgInfoList streamArgs;

        SoapySDR::ArgInfo buffersArg;
        buffersArg.key = "buffers";
        buffersArg.value = std::to_string(DEF_NUM_BUFFS);
        buffersArg.name = "Buffer Count";
        buffersArg.description = "Number of async USB buffers.";
        buffersArg.units = "buffers";
        buffersArg.type = SoapySDR::ArgInfo::INT;
        streamArgs.push_back(buffersArg);

        SoapySDR::ArgInfo lengthArg;
        lengthArg.key = "buflen";
        lengthArg.value = std::to_string(DEF_BUFF_LEN);
        lengthArg.name = "Buffer Length";
        lengthArg.description = "Number of bytes per USB buffer, the number must be a multiple of 1024.";
        lengthArg.units = "bytes";
        lengthArg.type = SoapySDR::ArgInfo::INT;
        streamArgs.push_back(lengthArg);

        SoapySDR::ArgInfo xfersArg;
        xfersArg.key = "transfers";
        xfersArg.value = "0";
        xfersArg.name = "Num Transfers";
        xfersArg.description = "Number of async USB transfers. Use 0 for automatic";
        xfersArg.units = "bytes";
        xfersArg.type = SoapySDR::ArgInfo::INT;
        xfersArg.range = SoapySDR::Range(0, 32);
        streamArgs.push_back(xfersArg);

        return streamArgs;
}

SoapySDR::Stream *bladeRF_SoapySDR::setupStream(
        const int direction,
        const std::string &format,
        const std::vector<size_t> &channels_,
        const SoapySDR::Kwargs &args)
{
        _is_beacon = (args.count("beacon") > 0);
        if (_is_beacon) {
                std::cout << "*** BEACON ***" << std::endl;
        } else {
                std::cout << "*** TAG *** " << std::endl;
        }

        auto channels = channels_;
        if (channels.empty()) channels.push_back(0);

        //check the channel configuration
        bladerf_channel_layout layout;
        if (channels.size() == 1 and channels.at(0) == 0)
        {
                layout = (direction == SOAPY_SDR_RX)?BLADERF_RX_X1:BLADERF_TX_X1;
                std::cout << "*** 1 ***" << std::endl;
        }
        else if (channels.size() == 2 and channels.at(0) == 0 and channels.at(1) == 1)
        {
                layout = (direction == SOAPY_SDR_RX)?BLADERF_RX_X2:BLADERF_TX_X2;
                std::cout << "*** 2 ***" << std::endl;
        }
        else
        {
                throw std::runtime_error("setupStream invalid channel selection");
        }

        //check the format
        if (format == "CF32") {}
        else if (format == "CS16") {}
        else throw std::runtime_error("setupStream invalid format " + format);

        //determine the number of buffers to allocate
        int numBuffs = (args.count("buffers") == 0)? 0 : atoi(args.at("buffers").c_str());
        if (numBuffs == 0) numBuffs = DEF_NUM_BUFFS;
        if (numBuffs == 1) numBuffs++;

        //determine the size of each buffer in samples
        int bufSize = (args.count("buflen") == 0)? 0 : atoi(args.at("buflen").c_str());
        if (bufSize == 0) bufSize = DEF_BUFF_LEN;
        if ((bufSize % 1024) != 0) bufSize = ((bufSize/1024) + 1) * 1024;

        //determine the number of active transfers
        int numXfers = (args.count("transfers") == 0)? 0 : atoi(args.at("transfers").c_str());
        if (numXfers == 0) numXfers = numBuffs/2;
        if (numXfers > numBuffs) numXfers = numBuffs; //cant have more than available buffers
        if (numXfers > 32) numXfers = 32; //libusb limit

        //setup the stream for sync tx/rx calls
        if (not _is_beacon) {
                numBuffs = 16;
                bufSize = 8192;
                numXfers = 8;
        }
        if (_is_beacon) {
                numBuffs = 16;
                bufSize = 8192;
                numXfers = 8;
        }
        int ret = bladerf_sync_config(
                _dev,
                layout,
                BLADERF_FORMAT_SC16_Q11_META,
                numBuffs,
                bufSize,
                numXfers,
                3500); //3.5 seconds timeout
        if (ret != 0)
        {
                SoapySDR::logf(SOAPY_SDR_ERROR, "bladerf_sync_config() returned %d", ret);
                throw std::runtime_error("setupStream() " + _err2str(ret));
        }

        //enable channels used in streaming
        for (const auto ch : channels)
        {
                ret = bladerf_enable_module(_dev, _toch(direction, ch), true);
                if (ret != 0)
                {
                        SoapySDR::logf(SOAPY_SDR_ERROR, "bladerf_enable_module(true) returned %d", ret);
                        throw std::runtime_error("setupStream() " + _err2str(ret));
                }
        }

        if (direction == SOAPY_SDR_RX)
        {
                _rxOverflow = false;
                _rxChans = channels;
                _rxFloats = (format == "CF32");
                _rxConvBuff = new int16_t[bufSize*2*_rxChans.size()];
                _rxBuffSize = bufSize;
                this->updateRxMinTimeoutMs();
        }

        if (direction == SOAPY_SDR_TX)
        {
                _txFloats = (format == "CF32");
                _txChans = channels;
                _txConvBuff = new int16_t[bufSize*2*_txChans.size()];
                _txBuffSize = bufSize;
        }

        return (SoapySDR::Stream *)(new int(direction));
}

void bladeRF_SoapySDR::closeStream(SoapySDR::Stream *stream)
{
        const int direction = *reinterpret_cast<int *>(stream);
        auto &chans = (direction == SOAPY_SDR_RX)?_rxChans:_txChans;

        //deactivate the stream here -- only call once
        for (const auto ch : chans)
        {
                const int ret = bladerf_enable_module(_dev, _toch(direction, ch), false);
                if (ret != 0)
                {
                        SoapySDR::logf(SOAPY_SDR_ERROR, "bladerf_enable_module(false) returned %s", _err2str(ret).c_str());
                        throw std::runtime_error("closeStream() " + _err2str(ret));
                }
        }
        chans.clear();

        //cleanup stream convert buffers
        if (direction == SOAPY_SDR_RX)
        {
                delete [] _rxConvBuff;
        }

        if (direction == SOAPY_SDR_TX)
        {
                delete [] _txConvBuff;
        }

        delete reinterpret_cast<int *>(stream);
}

size_t bladeRF_SoapySDR::getStreamMTU(SoapySDR::Stream *stream) const
{
        const int direction = *reinterpret_cast<int *>(stream);
        return (direction == SOAPY_SDR_RX)?_rxBuffSize:_txBuffSize;
}

int bladeRF_SoapySDR::activateStream(
        SoapySDR::Stream *stream,
        const int flags,
        const long long timeNs,
        const size_t numElems)
{
        const int direction = *reinterpret_cast<int *>(stream);

        if (direction == SOAPY_SDR_RX)
        {
                StreamMetadata cmd;
                cmd.flags = flags;
                cmd.timeNs = timeNs;
                cmd.numElems = numElems;
                _rxCmds.push(cmd);
        }

        if (direction == SOAPY_SDR_TX)
        {
                if (flags != 0) return SOAPY_SDR_NOT_SUPPORTED;
        }

        return 0;
}

int bladeRF_SoapySDR::deactivateStream(
        SoapySDR::Stream *stream,
        const int flags,
        const long long)
{
        const int direction = *reinterpret_cast<int *>(stream);
        if (flags != 0) return SOAPY_SDR_NOT_SUPPORTED;

        if (direction == SOAPY_SDR_RX)
        {
                //clear all commands when deactivating
                while (not _rxCmds.empty()) _rxCmds.pop();
        }

        if (direction == SOAPY_SDR_TX)
        {
                //in a burst -> end it
                if (_inTxBurst)
                {
                        //initialize metadata
                        bladerf_metadata md;
                        md.timestamp = 0;
                        md.flags = BLADERF_META_FLAG_TX_BURST_END;
                        md.status = 0;

                        //send the tx samples
                        _txConvBuff[0] = 0;
                        _txConvBuff[1] = 0;
                        bladerf_sync_tx(_dev, _txConvBuff, 1, &md, 100/*ms*/);
                }
                _inTxBurst = false;
        }

        return 0;
}

int bladeRF_SoapySDR::readStream(
        SoapySDR::Stream *,
        void * const *buffs,
        size_t numElems,
        int &flags,
        long long &timeNs,
        const long timeoutUs)
{
        //clip to the available conversion buffer size
        if (_is_beacon) {
                //numElems = std::min(numElems, _rxBuffSize);
        }

        //extract the front-most command
        //no command, this is a timeout...
        if (_rxCmds.empty()) return SOAPY_SDR_TIMEOUT;
        StreamMetadata &cmd = _rxCmds.front();

        //clear output metadata
        long long timeNsRx = timeNs;
        flags = 0;
        timeNs = 0;

        //return overflow status indicator
        if (_rxOverflow)
        {
                _rxOverflow = false;
                flags |= SOAPY_SDR_HAS_TIME;
                timeNs = _rxTicksToTimeNs(_rxNextTicks);
                return SOAPY_SDR_OVERFLOW;
        }

        //initialize metadata
        bladerf_metadata md;
        md.timestamp = 0;
        md.flags = 0;
        md.status = 0;

        //without a soapy sdr time flag, set the blade rf now flag
        if ((cmd.flags & SOAPY_SDR_HAS_TIME) == 0) md.flags |= BLADERF_META_FLAG_RX_NOW;
        md.timestamp = _timeNsToRxTicks(cmd.timeNs);
        if (cmd.numElems > 0) numElems = std::min(cmd.numElems, numElems);
        cmd.flags = 0; //clear flags for subsequent calls

        //prepare buffers
        void *samples = (void *)buffs[0];
        if (_rxFloats or _rxChans.size() == 2) samples = _rxConvBuff;

        bladerf_metadata my_md;
        bladerf_get_timestamp(_dev, BLADERF_RX, &my_md.timestamp);
        /*std::cout << "Read timestamp before RX: " << my_md.timestamp
                  << " in ns " << _rxTicksToTimeNs(my_md.timestamp)
                  << std::endl;*/

        if (timeNsRx != 0) {
                uint64_t timeNsRX_hw_ticks = _timeNsToRxTicks(timeNsRx);
                /*std::cout << "HW ns wanted RX in ticks: " << timeNsRX_hw_ticks
                          << " in ns " << timeNsRx
                          << std::endl;*/
                if (my_md.timestamp > timeNsRX_hw_ticks) {
                        std::cout << "Wanted time is in the passed!"
                                  << " Diff: -"
                                  << my_md.timestamp - timeNsRX_hw_ticks
                                  << std::endl;
                        //std::cout << "." << std::flush;
                        return 0;
                }
                md.flags = 0;
                //md.timestamp = my_md.timestamp + 1152000; // 150 ms in 7.68 Msps
                md.timestamp = _timeNsToRxTicks(timeNsRx);

        }

        /*std::cout << "Calculated want to RX at: " << md.timestamp
                  << " in ns " << _rxTicksToTimeNs(md.timestamp)
                  << std::endl;*/

        //recv the rx samples
        //const long timeoutMs = std::max(_rxMinTimeoutMs, timeoutUs/1000);
        const long timeoutMs = 5000;
        int ret = bladerf_sync_rx(_dev, samples, numElems*_rxChans.size(), &md, timeoutMs);
        if (ret == BLADERF_ERR_TIMEOUT) return SOAPY_SDR_TIMEOUT;
        if (ret == BLADERF_ERR_TIME_PAST) return SOAPY_SDR_TIME_ERROR;
        if (ret != 0)
        {
                //any error when this is a finite burst causes the command to be removed
                if (cmd.numElems > 0) _rxCmds.pop();
                SoapySDR::logf(SOAPY_SDR_ERROR, "bladerf_sync_rx() returned %s", _err2str(ret).c_str());
                return SOAPY_SDR_STREAM_ERROR;
        }

        /*std::cout << "RX'd at: " << md.timestamp
                  << " in ns " << _rxTicksToTimeNs(md.timestamp)
                  << std::endl;*/

        /*bladerf_get_timestamp(_dev, BLADERF_RX, &my_md.timestamp);
        std::cout << "Read timestamp after RX: " << my_md.timestamp
                  << " in ns " << _rxTicksToTimeNs(my_md.timestamp)
                  << std::endl;*/

        //actual count is number of samples in total all channels
        numElems = md.actual_count / _rxChans.size();

        //perform the int16 to float conversion
        if (_rxFloats and _rxChans.size() == 1)
        {
                float *output = (float *)buffs[0];
                for (size_t i = 0; i < 2 * numElems; i++)
                {
                        output[i] = float(_rxConvBuff[i])/2048;
                }
        }
        else if (not _rxFloats and _rxChans.size() == 2)
        {
                int16_t *output0 = (int16_t *)buffs[0];
                int16_t *output1 = (int16_t *)buffs[1];
                for (size_t i = 0; i < 4 * numElems;)
                {
                        *(output0++) = _rxConvBuff[i++];
                        *(output0++) = _rxConvBuff[i++];
                        *(output1++) = _rxConvBuff[i++];
                        *(output1++) = _rxConvBuff[i++];
                }
        }
        else if (_rxFloats and _rxChans.size() == 2)
        {
                float *output0 = (float *)buffs[0];
                float *output1 = (float *)buffs[1];
                for (size_t i = 0; i < 4 * numElems;)
                {
                        *(output0++) = float(_rxConvBuff[i++])/2048;
                        *(output0++) = float(_rxConvBuff[i++])/2048;
                        *(output1++) = float(_rxConvBuff[i++])/2048;
                        *(output1++) = float(_rxConvBuff[i++])/2048;
                }
        }

        //unpack the metadata
        flags |= SOAPY_SDR_HAS_TIME;
        timeNs = _rxTicksToTimeNs(md.timestamp);

        //parse the status
        if ((md.status & BLADERF_META_STATUS_OVERRUN) != 0)
        {
                //SoapySDR::log(SOAPY_SDR_SSI, "0");
                //_rxOverflow = true;
        }

        //consume from the command if this is a finite burst
        if (cmd.numElems > 0)
        {
                cmd.numElems -= numElems;
                if (cmd.numElems == 0) _rxCmds.pop();
        }

        _rxNextTicks = md.timestamp + numElems;
        return numElems;
}

int bladeRF_SoapySDR::writeStream(
        SoapySDR::Stream *,
        const void * const *buffs,
        size_t numElems,
        int &flags,
        const long long timeNs,
        const long timeoutUs)
{
        //clear EOB when the last sample will not be transmitted
        if (numElems > _txBuffSize) flags &= ~(SOAPY_SDR_END_BURST);

        //clip to the available conversion buffer size
        numElems = std::min(numElems, _txBuffSize);

        //initialize metadata
        bladerf_metadata md;
        md.timestamp = 0;
        md.flags = 0;
        md.status = 0;

        //time and burst start
        if (_inTxBurst)
        {
                if ((flags & SOAPY_SDR_HAS_TIME) != 0)
                {
                        md.timestamp = _timeNsToTxTicks(timeNs);
                        md.flags |= BLADERF_META_FLAG_TX_UPDATE_TIMESTAMP;
                        _txNextTicks = md.timestamp;
                }
        }
        else
        {
                md.flags |= BLADERF_META_FLAG_TX_BURST_START;
                if ((flags & SOAPY_SDR_HAS_TIME) != 0)
                {
                        md.timestamp = _timeNsToTxTicks(timeNs);
                }
                else
                {
                        md.flags |= BLADERF_META_FLAG_TX_NOW;
                        bladerf_get_timestamp(_dev, BLADERF_TX, &md.timestamp);
                }
                _txNextTicks = md.timestamp;
        }

        //end of burst
        if ((flags & SOAPY_SDR_END_BURST) != 0)
        {
                md.flags |= BLADERF_META_FLAG_TX_BURST_END;
        }

        //prepare buffers
        void *samples = (void *)buffs[0];
        if (_txFloats or _txChans.size() == 2) samples = _txConvBuff;

        //perform the float to int16 conversion
        if (_txFloats and _txChans.size() == 1)
        {
                float *input = (float *)buffs[0];
                for (size_t i = 0; i < 2 * numElems; i++)
                {
                        _txConvBuff[i] = int16_t(input[i]*2048);
                }
        }
        else if (not _txFloats and _txChans.size() == 2)
        {
                int16_t *input0 = (int16_t *)buffs[0];
                int16_t *input1 = (int16_t *)buffs[1];
                for (size_t i = 0; i < 4 * numElems;)
                {
                        _txConvBuff[i++] = *(input0++);
                        _txConvBuff[i++] = *(input0++);
                        _txConvBuff[i++] = *(input1++);
                        _txConvBuff[i++] = *(input1++);
                }
        }
        else if (_txFloats and _txChans.size() == 2)
        {
                float *input0 = (float *)buffs[0];
                float *input1 = (float *)buffs[1];
                for (size_t i = 0; i < 4 * numElems;)
                {
                        _txConvBuff[i++] = int16_t(*(input0++)*2048);
                        _txConvBuff[i++] = int16_t(*(input0++)*2048);
                        _txConvBuff[i++] = int16_t(*(input1++)*2048);
                        _txConvBuff[i++] = int16_t(*(input1++)*2048);
                }
        }

        //send the tx samples
        int ret = bladerf_sync_tx(_dev, samples, numElems*_txChans.size(), &md, timeoutUs/1000);
        if (ret == BLADERF_ERR_TIMEOUT) return SOAPY_SDR_TIMEOUT;
        if (ret == BLADERF_ERR_TIME_PAST) return SOAPY_SDR_TIME_ERROR;
        if (ret != 0)
        {
                SoapySDR::logf(SOAPY_SDR_ERROR, "bladerf_sync_tx() returned %s", _err2str(ret).c_str());
                return SOAPY_SDR_STREAM_ERROR;
        }
        _txNextTicks += numElems;

        //always in a burst after successful tx
        _inTxBurst = true;

        //parse the status
        if ((md.status & BLADERF_META_STATUS_UNDERRUN) != 0)
        {
                SoapySDR::log(SOAPY_SDR_SSI, "U");
                StreamMetadata resp;
                resp.flags = 0;
                resp.code = SOAPY_SDR_UNDERFLOW;
                _txResps.push(resp);
        }

        //end burst status message
        if ((flags & SOAPY_SDR_END_BURST) != 0)
        {
                StreamMetadata resp;
                resp.flags = SOAPY_SDR_END_BURST | SOAPY_SDR_HAS_TIME;
                resp.timeNs = this->_txTicksToTimeNs(_txNextTicks);
                resp.code = 0;
                _txResps.push(resp);
                _inTxBurst = false;
        }

        return numElems;
}

int bladeRF_SoapySDR::readStreamStatus(
        SoapySDR::Stream *stream,
        size_t &,
        int &flags,
        long long &timeNs,
        const long timeoutUs
        )
{
        const int direction = *reinterpret_cast<int *>(stream);
        if (direction == SOAPY_SDR_RX) return SOAPY_SDR_NOT_SUPPORTED;

        //wait for an event to be ready considering the timeout and time
        //this is an emulation by polling and waiting on the hardware time
        long long timeNowNs = this->getHardwareTime();
        const long long exitTimeNs = timeNowNs + (timeoutUs*1000);
        while (true)
        {
                //no status to report, sleep for a bit
                if (_txResps.empty()) goto pollSleep;

                //no time on the current status, done waiting...
                if ((_txResps.front().flags & SOAPY_SDR_HAS_TIME) == 0) break;

                //current status time expired, done waiting...
                if (_txResps.front().timeNs < timeNowNs) break;

                //sleep a bit, never more than time remaining
        pollSleep:
                usleep(std::min<long>(1000, (exitTimeNs-timeNowNs)/1000));

                //check for timeout expired
                timeNowNs = this->getHardwareTime();
                if (exitTimeNs < timeNowNs) return SOAPY_SDR_TIMEOUT;
        }

        //extract the most recent status event
        if (_txResps.empty()) return SOAPY_SDR_TIMEOUT;
        StreamMetadata resp = _txResps.front();
        _txResps.pop();

        //load the output from the response
        flags = resp.flags;
        timeNs = resp.timeNs;
        return resp.code;
}
