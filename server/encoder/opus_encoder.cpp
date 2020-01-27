/***
        This file is part of snapcast
        Copyright (C) 2015  Hannes Ellinger

        This program is free software: you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation, either version 3 of the License, or
        (at your option) any later version.

        This program is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#include "opus_encoder.hpp"
#include "common/aixlog.hpp"
#include "common/snap_exception.hpp"
#include "common/str_compat.hpp"
#include "common/utils/string_utils.hpp"

using namespace std;

namespace encoder
{

#define ID_OPUS 0x4F505553
static constexpr opus_int32 const_min_bitrate = 6000;
static constexpr opus_int32 const_max_bitrate = 512000;

namespace
{
template <typename T>
void assign(void* pointer, T val)
{
    T* p = (T*)pointer;
    *p = val;
}
} // namespace


OpusEncoder::OpusEncoder(const std::string& codecOptions) : Encoder(codecOptions), enc_(nullptr)
{
    headerChunk_ = make_unique<msg::CodecHeader>("opus");
}


OpusEncoder::~OpusEncoder()
{
    if (enc_ != nullptr)
        opus_encoder_destroy(enc_);
}


std::string OpusEncoder::getAvailableOptions() const
{
    return "BITRATE:[" + cpt::to_string(const_min_bitrate) + " - " + cpt::to_string(const_max_bitrate) + "|MAX|AUTO],COMPLEXITY:[1-10]";
}


std::string OpusEncoder::getDefaultOptions() const
{
    return "BITRATE:192000,COMPLEXITY:10";
}


std::string OpusEncoder::name() const
{
    return "opus";
}


void OpusEncoder::initEncoder()
{
    // Opus is quite restrictive in sample rate and bit depth
    // It can handle mono signals, but we will check for stereo
    if ((sampleFormat_.rate != 48000) || (sampleFormat_.bits != 16) || (sampleFormat_.channels != 2))
        throw SnapException("Opus sampleformat must be 48000:16:2");

    opus_int32 bitrate = 192000;
    opus_int32 complexity = 10;

    // parse options: bitrate and complexity
    auto options = utils::string::split(codecOptions_, ',');
    for (const auto& option : options)
    {
        auto kv = utils::string::split(option, ':');
        if (kv.size() == 2)
        {
            if (kv.front() == "BITRATE")
            {
                if (kv.back() == "MAX")
                    bitrate = OPUS_BITRATE_MAX;
                else if (kv.back() == "AUTO")
                    bitrate = OPUS_AUTO;
                else
                {
                    try
                    {
                        bitrate = cpt::stoi(kv.back());
                        if ((bitrate < const_min_bitrate) || (bitrate > const_max_bitrate))
                            throw SnapException("Opus bitrate must be between " + cpt::to_string(const_min_bitrate) + " and " +
                                                cpt::to_string(const_max_bitrate));
                    }
                    catch (const std::invalid_argument&)
                    {
                        throw SnapException("Opus error parsing bitrate (must be between " + cpt::to_string(const_min_bitrate) + " and " +
                                            cpt::to_string(const_max_bitrate) + "): " + kv.back());
                    }
                }
            }
            else if (kv.front() == "COMPLEXITY")
            {
                try
                {
                    complexity = cpt::stoi(kv.back());
                    if ((complexity < 1) || (complexity > 10))
                        throw SnapException("Opus complexity must be between 1 and 10");
                }
                catch (const std::invalid_argument&)
                {
                    throw SnapException("Opus error parsing complexity (must be between 1 and 10): " + kv.back());
                }
            }
            else
                throw SnapException("Opus unknown option: " + kv.front());
        }
        else
            throw SnapException("Opus error parsing options: " + codecOptions_);
    }

    LOG(INFO) << "Opus bitrate: " << bitrate << " bps, complexity: " << complexity << "\n";

    int error;
    enc_ = opus_encoder_create(sampleFormat_.rate, sampleFormat_.channels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
    if (error != 0)
    {
        throw SnapException("Failed to initialize Opus encoder: " + std::string(opus_strerror(error)));
    }

    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(complexity));

    // create some opus pseudo header to let the decoder know about the sample format
    headerChunk_->payloadSize = 12;
    headerChunk_->payload = (char*)malloc(headerChunk_->payloadSize);
    char* payload = headerChunk_->payload;
    assign(payload, SWAP_32(ID_OPUS));
    assign(payload + 4, SWAP_32(sampleFormat_.rate));
    assign(payload + 8, SWAP_16(sampleFormat_.bits));
    assign(payload + 10, SWAP_16(sampleFormat_.channels));

    remainder_ = std::make_unique<msg::PcmChunk>(sampleFormat_, 10);
    remainder_max_size_ = remainder_->payloadSize;
    remainder_->payloadSize = 0;
}


// Opus encoder can only handle chunk sizes of:
//   5,  10,  20,   40,   60 ms
// 240, 480, 960, 1920, 2880 frames
// We will split the chunk into encodable sizes and store any remaining data in the remainder_ buffer
// and encode the buffer content in the next iteration
void OpusEncoder::encode(const msg::PcmChunk* chunk)
{
    // LOG(TRACE) << "encode " << chunk->duration<std::chrono::milliseconds>().count() << "ms\n";
    uint32_t offset = 0;

    // check if there is something left from the last call to encode and fill the remainder buffer to
    // an encodable size of 10ms
    if (remainder_->payloadSize > 0)
    {
        offset = std::min(static_cast<uint32_t>(remainder_max_size_ - remainder_->payloadSize), chunk->payloadSize);
        memcpy(remainder_->payload + remainder_->payloadSize, chunk->payload, offset);
        // LOG(TRACE) << "remainder buffer size: " << remainder_->payloadSize << "/" << remainder_max_size_ << ", appending " << offset << " bytes\n";
        remainder_->payloadSize += offset;

        if (remainder_->payloadSize < remainder_max_size_)
        {
            LOG(DEBUG) << "not enough data to encode (" << remainder_->payloadSize << " of " << remainder_max_size_ << " bytes)"
                       << "\n";
            return;
        }
        encode(chunk->format, remainder_->payload, remainder_->payloadSize);
        remainder_->payloadSize = 0;
    }

    // encode greedy 60ms, 40ms, 20ms, 10ms chunks
    std::vector<size_t> chunk_durations{60, 40, 20, 10};
    for (const auto duration : chunk_durations)
    {
        auto ms2bytes = [this](size_t ms) { return (ms * sampleFormat_.msRate() * sampleFormat_.frameSize); };
        uint32_t bytes = ms2bytes(duration);
        while (chunk->payloadSize - offset >= bytes)
        {
            // LOG(TRACE) << "encoding " << duration << "ms (" << bytes << "), offset: " << offset << ", chunk size: " << chunk->payloadSize - offset << "\n";
            encode(chunk->format, chunk->payload + offset, bytes);
            offset += bytes;
        }
        if (chunk->payloadSize == offset)
            break;
    }

    // something is left (must be less than 10ms)
    if (chunk->payloadSize > offset)
    {
        memcpy(remainder_->payload + remainder_->payloadSize, chunk->payload + offset, chunk->payloadSize - offset);
        remainder_->payloadSize = chunk->payloadSize - offset;
    }
}


void OpusEncoder::encode(const SampleFormat& format, const char* data, size_t size)
{
    // void* buffer;
    // LOG(INFO) << "frames: " << chunk->readFrames(buffer, std::chrono::milliseconds(10)) << "\n";
    int samples_per_channel = size / format.frameSize;
    if (encoded_.size() < size)
        encoded_.resize(size);

    opus_int32 len = opus_encode(enc_, (opus_int16*)data, samples_per_channel, encoded_.data(), size);
    // LOG(TRACE) << "Encode " << samples_per_channel << " frames, size " << size << " bytes, encoded: " << len << " bytes" << '\n';

    if (len > 0)
    {
        // copy encoded data to chunk
        auto* opusChunk = new msg::PcmChunk(format, 0);
        opusChunk->payloadSize = len;
        opusChunk->payload = (char*)realloc(opusChunk->payload, opusChunk->payloadSize);
        memcpy(opusChunk->payload, encoded_.data(), len);
        listener_->onChunkEncoded(this, opusChunk, (double)samples_per_channel / ((double)sampleFormat_.rate / 1000.));
    }
    else
    {
        LOG(ERROR) << "Failed to encode chunk: " << opus_strerror(len) << ", samples / channel: " << samples_per_channel << ", bytes:  " << size << '\n';
    }
}

} // namespace encoder
