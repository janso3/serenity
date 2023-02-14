/*
 * Copyright (c) 2023, Julian Offenhäuser <offenhaeuser@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibAudio/Loader.h>

namespace Audio {

class ModLoaderPlugin : public LoaderPlugin {
public:
    explicit ModLoaderPlugin(NonnullOwnPtr<SeekableStream> stream);
    static Result<NonnullOwnPtr<ModLoaderPlugin>, LoaderError> create(StringView path);
    static Result<NonnullOwnPtr<ModLoaderPlugin>, LoaderError> create(Bytes buffer);

    virtual LoaderSamples get_more_samples(size_t max_samples_to_read_from_input = 128 * KiB) override;

    virtual MaybeLoaderError reset() override { return seek(0); }

    // sample_index 0 is the start of the raw audio sample data
    // within the file/stream.
    virtual MaybeLoaderError seek(int sample_index) override;

    virtual int loaded_samples() override { return static_cast<int>(m_loaded_samples); }
    virtual int total_samples() override { return static_cast<int>(m_total_samples); }
    virtual u32 sample_rate() override { return m_sample_rate; }
    virtual u16 num_channels() override { return m_num_channels; }
    virtual DeprecatedString format_name() override { return m_format_name; }
    virtual PcmSampleFormat pcm_format() override { return m_sample_format; }

private:
    static constexpr size_t PATTERN_DATA_OFFSET = 1084;
    static constexpr size_t MAX_CHANNELS = 32;

    struct Instrument {
        u8 volume;
        u8 fine_tune;
        u8 loop_start;
        u8 loop_length;
        ByteBuffer sample_data;
    };

    struct Note {
        u16 key;
        u8 instrument;
        u8 effect;
        u8 parameter;
    };

    struct Pattern {
        Vector<Note> notes;
    };

    struct Channel {
        Note note;
        size_t sample_offset;
        u16 period;
        u8 volume;
        u8 panning;
        u8 fine_tune;
    };

    struct PlaybackState {
        Array<Channel, MAX_CHANNELS> channels;
        u16 pattern;
        u16 speed;
        u16 row;
        u16 tick;
        u16 volume;
    };

    MaybeLoaderError initialize();
    MaybeLoaderError parse();

    void reset_playback_parameters();
    void note_trigger(Channel&);
    void channel_tick(Channel&);
    void tick();

    DeprecatedString m_format_name;

    PlaybackState m_state;

    Array<Instrument, 32> m_instruments;
    Array<u8, 128> m_order_table;
    Array<Pattern, 128> m_patterns;

    u16 m_song_length;
    u16 m_song_restart;
    u16 m_num_module_channels;

    u32 m_sample_rate { 44100 };
    u16 m_num_channels { 2 };
    PcmSampleFormat m_sample_format { PcmSampleFormat::Int16 };
    size_t m_byte_offset_of_data_samples { 0 };

    size_t m_loaded_samples { 0 };
    size_t m_total_samples { 0 };
};

}
