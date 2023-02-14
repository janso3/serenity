/*
 * Copyright (c) 2023, Julian Offenhäuser <offenhaeuser@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ModLoader.h"
#include <AK/Endian.h>
#include <AK/MemoryStream.h>
#include <LibCore/File.h>

namespace Audio {

ModLoaderPlugin::ModLoaderPlugin(NonnullOwnPtr<SeekableStream> stream)
    : LoaderPlugin(move(stream))
{
}

Result<NonnullOwnPtr<ModLoaderPlugin>, LoaderError> ModLoaderPlugin::create(StringView path)
{
    auto stream = LOADER_TRY(Core::BufferedFile::create(LOADER_TRY(Core::File::open(path, Core::File::OpenMode::Read))));
    auto loader = make<ModLoaderPlugin>(move(stream));

    LOADER_TRY(loader->initialize());

    return loader;
}

Result<NonnullOwnPtr<ModLoaderPlugin>, LoaderError> ModLoaderPlugin::create(Bytes buffer)
{
    auto stream = LOADER_TRY(try_make<FixedMemoryStream>(buffer));
    auto loader = make<ModLoaderPlugin>(move(stream));

    LOADER_TRY(loader->initialize());

    return loader;
}

MaybeLoaderError ModLoaderPlugin::initialize()
{
    LOADER_TRY(parse());

    return {};
}

LoaderSamples ModLoaderPlugin::get_more_samples(size_t max_samples_to_read_from_input)
{
    FixedArray<Sample> samples = LOADER_TRY(FixedArray<Sample>::create(max_samples_to_read_from_input));

    return samples;
}

MaybeLoaderError ModLoaderPlugin::seek(int sample_index)
{
    (void)sample_index;

    return {};
}

MaybeLoaderError ModLoaderPlugin::parse()
{
    auto read_u8 = [&]() -> ErrorOr<u8, LoaderError> {
        u8 value;
        LOADER_TRY(m_stream->read(Bytes { &value, 1 }));
        return value;
    };

    auto read_u16 = [&]() -> ErrorOr<u16, LoaderError> {
        u16 value;
        LOADER_TRY(m_stream->read(Bytes { &value, 2 }));
        return AK::convert_between_host_and_big_endian(value);
    };

    auto read_u32 = [&]() -> ErrorOr<u32, LoaderError> {
        u32 value;
        LOADER_TRY(m_stream->read(Bytes { &value, 4 }));
        return AK::convert_between_host_and_big_endian(value);
    };

    // Determine the format version and number of channels.
    // This also serves as an early return for formats we don't handle.
    LOADER_TRY(m_stream->seek(1080, SeekMode::SetPosition));

    auto tracker_id = LOADER_TRY(read_u32());
    switch (tracker_id) {
    case 0x4d2e4b2e: // M.K.
        m_format_name = "Protracker M.K."sv;
        m_num_module_channels = 4;
        break;
    case 0x4d214b21: // M!K!
        m_format_name = "Protracker M!K!"sv;
        m_num_module_channels = 4;
        break;
    case 0x464c5434: // FLT4
        m_format_name = "Startrekker 4CH"sv;
        m_num_module_channels = 4;
        break;
    case 0x464c5438: // FLT8
        m_format_name = "Startrekker 8CH"sv;
        m_num_module_channels = 8;
        break;
    default: {
        if ((tracker_id & 0xffff) == 0x484e) {
            // xCHN VERIFY
            m_num_module_channels = (tracker_id >> 24) - 48;
        } else if ((tracker_id & 0xffff) == 0x4348) {
            // xxCH
            m_num_module_channels = 10 * ((tracker_id >> 24) - 48) + (((tracker_id >> 16) & 0xff) - 48);
        } else {
            return LoaderError("Unknown tracker signature");
        }
        m_format_name = DeprecatedString::formatted("FastTracker {}CH", m_num_module_channels);

        break;
    }
    }

    VERIFY(m_num_module_channels <= MAX_CHANNELS);

    LOADER_TRY(m_stream->seek(0, SeekMode::SetPosition));

    char song_name[20];
    LOADER_TRY(m_stream->read(Bytes { song_name, sizeof(song_name) }));
    song_name[19] = 0;

    // Read instrument info.
    constexpr size_t NUM_INSTRUMENTS = 31;

    Array<u16, NUM_INSTRUMENTS> sample_lengths;
    for (size_t instrument_index = 0; instrument_index < NUM_INSTRUMENTS; ++instrument_index) {
        Instrument& instrument = m_instruments[instrument_index];

        char sample_name[22];
        LOADER_TRY(m_stream->read(Bytes { sample_name, sizeof(sample_name) }));
        sample_name[21] = 0;

        sample_lengths[instrument_index] = LOADER_TRY(read_u16());
        instrument.fine_tune = LOADER_TRY(read_u8()) & 0x7f;
        instrument.volume = LOADER_TRY(read_u8());
        instrument.loop_start = LOADER_TRY(read_u16());
        instrument.loop_length = LOADER_TRY(read_u16());
    }

    m_song_length = LOADER_TRY(read_u8()) & 0x7f;
    m_song_restart = LOADER_TRY(read_u8()) & 0x7f;

    // Load the pattern order table.
    // We determine the number of stored patterns by looking
    // for the highest pattern number in use.
    u8 num_patterns = 0;
    for (size_t order_index = 0; order_index < 128; order_index++) {
        auto pattern = LOADER_TRY(read_u8()) & 0x7f;
        if (pattern >= num_patterns)
            num_patterns = pattern + 1;
        m_order_table[order_index] = pattern;
    }

    VERIFY(num_patterns <= 128);

    // Load the pattern data.
    LOADER_TRY(m_stream->seek(1084, SeekMode::SetPosition));

    for (size_t pattern_index = 0; pattern_index < num_patterns; ++pattern_index) {
        Pattern& pattern = m_patterns[pattern_index];
        LOADER_TRY(pattern.notes.try_resize(m_num_module_channels * 64));

        for (size_t row_index = 0; row_index < 64; ++row_index) {
            for (size_t channel_index = 0; channel_index < m_num_module_channels; ++channel_index) {
                Note& note = pattern.notes[row_index * m_num_module_channels + channel_index];

                auto raw_note = LOADER_TRY(read_u32());

                auto effect = (raw_note >> 8) & 0xf;
                auto parameter = raw_note & 0xff;

                if (effect == 0xe) {
                    // This is an extended effect.
                    effect = 0x10 | (parameter >> 4);
                    parameter &= 0xf;
                }

                note.effect = effect;
                note.parameter = parameter;
                note.key = (raw_note >> 16) & 0xfff;
                note.instrument = ((raw_note >> 24) & 0xf0) | ((raw_note >> 12) & 0xf);
            }
        }
    }

    // dbgln("Offset now: {}, should be: {}", LOADER_TRY(m_stream->tell()), 1084 + 4 * m_num_module_channels * 64 * num_patterns);

    // Read and convert sample data.
    // LOADER_TRY(m_stream->seek(4 * m_num_module_channels * 64 * num_patterns, SeekMode::FromCurrentPosition));
    for (size_t instrument_index = 0; instrument_index < NUM_INSTRUMENTS; ++instrument_index) {
        Instrument& instrument = m_instruments[instrument_index];
        auto sample_length_in_bytes = static_cast<size_t>(sample_lengths[instrument_index]) * 2;
        instrument.sample_data = LOADER_TRY(ByteBuffer::create_uninitialized(sample_length_in_bytes));
        LOADER_TRY(m_stream->read(instrument.sample_data));
    }

    reset_playback_parameters();

    for (int i = 0; i < 64; ++i) {
        tick();
        m_state.row++;
    }

    return {};
}

void ModLoaderPlugin::reset_playback_parameters()
{
    m_state.tick = 1;
    m_state.speed = 6;
    m_state.volume = 64;
    m_state.pattern = 0;
    m_state.row = 0;
}

void ModLoaderPlugin::note_trigger(Channel& channel)
{
    auto& note = channel.note;

    // Set instrument.
    if (note.instrument && note.instrument <= m_instruments.size()) {
        auto& instrument = m_instruments[note.instrument];
        channel.volume = instrument.volume;
        channel.sample_offset = 0;
    }

    // Key change.
    if (note.key) {
    }
}

void ModLoaderPlugin::channel_tick(Channel& channel)
{
    // out("| {:04x} {:02x} {:01x}{:02x} ", note.key, note.instrument, note.effect, note.parameter);

    if (channel.note.parameter > 0) {
        note_trigger(channel);
    }

    switch (channel.note.effect) {
    default:
        break;
    }
}

void ModLoaderPlugin::tick()
{
    VERIFY(m_state.pattern <= 128);
    auto pattern_index = m_order_table[m_state.pattern];

    VERIFY(pattern_index <= 128);
    auto& pattern = m_patterns[pattern_index];

    // out("{:02x}: ", m_state.row);
    for (size_t channel_index = 0; channel_index < m_num_module_channels; ++channel_index) {
        auto& channel = m_state.channels[channel_index];

        size_t note_index = m_state.row * m_num_module_channels + channel_index;
        channel.note = pattern.notes[note_index];

        channel_tick(channel);
    }
    // out("\n");
}

}
