#pragma once
#include <memory.h>
#include <math.h>

#include "BitReader.hpp"
#include "ZRawImageBlockLine.hpp"
#include "Tools.hpp"

class NotImplemented : public std::logic_error
{
public:
    NotImplemented() : std::logic_error("Function not yet implemented"){};
};

class ZRawImageLineBlockReader
{
public:
    struct Parameters
    {
        uint16_t default_pix_value;

        int max_allowed_pixel_value;
        int max_allowed_raw_value;

        int max_values_count;
        int blocks_count;

        bool stride;
        int align_mode;

        bool lossless;

        int bitdepth_real;

        uint32_t bayer_pattern;

        int noise_level_1;
        int noise_level_2;

        uint32_t noise_level_distance;
    };

    ZRawImageLineBlockReader(Parameters param)
        : _param(param), _read_values_count(0), _current_line_index(0),
          _current_block_index(0), _noise_level(0), _noise_less_than_distance_count(0),
          _line_a(param.blocks_count, param.max_values_count, param.default_pix_value),
          _line_bc(param.blocks_count, param.max_values_count, param.default_pix_value),
          _line_a_prev(param.blocks_count, param.max_values_count, param.default_pix_value),
          _line_b_prev(param.blocks_count, param.max_values_count, param.default_pix_value),
          _line_c_prev(param.blocks_count, param.max_values_count, param.default_pix_value)
    {
        memset(noise_levels, 0x00, sizeof(noise_levels));

        _reinitializeContexts();
    }

    void _reinitializeContexts()
    {
        for (int i = 0; i < 3; ++i)
        {
            _context_a.last_new_read_values[i] = _param.default_pix_value;
            _context_a.last_old_read_values[i] = _param.default_pix_value;

            _context_b.last_new_read_values[i] = _param.default_pix_value;
            _context_b.last_old_read_values[i] = _param.default_pix_value;
        }

        _context_a.g = 4;
        _context_b.g = 4;
    }

    void ReadLine(BitReader *reader)
    {
        __reader = reader;

        bool is_last_block_read = false;
        while (!is_last_block_read)
            is_last_block_read = ReadNext();
    }

    bool ReadNext()
    {
        if (_current_block_index >= _param.blocks_count)
            return true;

        uint32_t header_value = _readBlockHeader();
        _line_a.HeaderValues()[_current_block_index] = header_value;
        _line_bc.HeaderValues()[_current_block_index] = header_value;

        _initBlockParameters();

        if (_decoding_mode == 0x100000001)
            _readNextBlockRawMode(_current_line_index > 0);
        else
            _readNextBlockVariableLengthMode(_current_line_index > 0);

        // Increase block counter
        ++_current_block_index;

        // If last block is just read
        if (_current_block_index >= _param.blocks_count)
        {
            // Alignment after last block
            if (_param.stride)
                _alignLastBlock();

            // Last block is read
            return true;
        }

        // Not last block is read
        return false;
    }

    void FinalizeLine()
    {
        _line_a_prev = _line_a;
        if (_is_upper_field_line())
            _line_b_prev = _line_bc;
        else
            _line_c_prev = _line_bc;

        _post_process();

        ++_current_line_index;

        // Reset block counter
        _current_block_index = 0;

        // Reset line values counter
        _read_values_count = 0;

        // Reset pixel noise counter
        _noise_less_than_distance_count = 0;

        // Reinit decoding contexts
        _reinitializeContexts();
    }

    std::vector<uint16_t> LineA()
    {
        return _line_a.Line();
    }

    std::vector<uint16_t> LineB()
    {
        return _line_bc.Line();
    }

private:
    BitReader &_reader()
    {
        return *__reader;
    }

    void _alignLastBlock()
    {
        _reader().BitAlignTo(_param.align_mode == 1 ? 256 : 128);
    }

    uint32_t _readBlockHeader()
    {
        if (_param.lossless)
            _bitdepth_diff = 0;
        else if (_read_values_count > 0)
        {
            // Unknown flag
            auto flag0 = _reader().ReadBits(1);
            if (flag0)
            {
                // Unknown value
                switch (_reader().ReadBits(2))
                {
                case 0:
                    _bitdepth_diff = _bitdepth_diff - 2;
                    break;
                case 1:
                    _bitdepth_diff = _bitdepth_diff - 1;
                    break;
                case 2:
                    _bitdepth_diff = _bitdepth_diff + 1;
                    break;
                case 3:
                    _bitdepth_diff = _bitdepth_diff + 2;
                    break;
                }
            }
            //else
            //    _bitdepth_diff = _bitdepth_diff; // (no changes)
        }
        else
            _bitdepth_diff = _reader().ReadBits(4);

        // Read block decoding mode
        _decoding_mode = _reader().ReadBits(1) ? 0x100000001 : 0x0;

        return _bitdepth_diff;
    }

    void _readNextBlockVariableLengthMode(bool isPrevLineDependant)
    {
        for (int i = 0; i < ZRAW_LINE_BLOCK_SIZE && _read_values_count < _param.max_values_count; ++i, ++_read_values_count)
        {
            // Set pixel values from prev line
            _context_a.last_old_read_values[0] = isPrevLineDependant ? _line_a_prev[_current_block_index][i] : _param.default_pix_value;
            _context_b.last_old_read_values[0] = isPrevLineDependant ?
                (_is_upper_field_line() ? _line_b_prev : _line_c_prev)[_current_block_index][i] : _param.default_pix_value;

            _processComponentPair();

            _collectNoiseLevelStatistics(_context_a, _param.noise_level_distance);

            // Shift last read values in contexts
            for (int p = 2; p > 0; --p)
            {
                // For the first component
                _context_a.last_new_read_values[p] = _context_a.last_new_read_values[p - 1];
                _context_a.last_old_read_values[p] = _context_a.last_old_read_values[p - 1];

                // For the second component
                _context_b.last_new_read_values[p] = _context_b.last_new_read_values[p - 1];
                _context_b.last_old_read_values[p] = _context_b.last_old_read_values[p - 1];
            }

            // Save last read values
            _line_a[_current_block_index][i] = _context_a.last_new_read_values[0];
            _line_bc[_current_block_index][i] = _context_b.last_new_read_values[0];
        }
    }

    void _readNextBlockRawMode(bool isPrevLineDependant)
    {
        for (int i = 0; i < ZRAW_LINE_BLOCK_SIZE && _read_values_count < _param.max_values_count; ++i, ++_read_values_count)
        {
            // Set pixel values from prev line
            _context_a.last_old_read_values[0] = isPrevLineDependant ? _line_a_prev[_current_block_index][i] : _param.default_pix_value;
            _context_b.last_old_read_values[0] = isPrevLineDependant ?
                (_is_upper_field_line() ? _line_b_prev : _line_c_prev)[_current_block_index][i] : _param.default_pix_value;

            auto val1 = _reader().ReadBits(_param.bitdepth_real - _parameters_raw_mode.a);
            auto val2 = _reader().ReadBits(_param.bitdepth_real - _parameters_raw_mode.a);

            // Save new read pixel value
            _context_a.last_new_read_values[0] = val1 << _parameters_raw_mode.a;
            auto old = _context_b.last_new_read_values[0];
            _context_b.last_new_read_values[0] = val2 << _parameters_raw_mode.a;

            _collectNoiseLevelStatistics(_context_a, _param.noise_level_distance);

            // Shift last read values in contexts
            for (int p = 2; p > 0; --p)
            {
                // For the first component
                _context_a.last_new_read_values[p] = _context_a.last_new_read_values[p - 1];
                _context_a.last_old_read_values[p] = _context_a.last_old_read_values[p - 1];

                // For the second component
                _context_b.last_new_read_values[p] = _context_b.last_new_read_values[p - 1];
                _context_b.last_old_read_values[p] = _context_b.last_old_read_values[p - 1];
            }

            // Save last read values
            _line_a[_current_block_index][i] = _context_a.last_new_read_values[0];
            _line_bc[_current_block_index][i] = _context_b.last_new_read_values[0];

            _context_b.last_new_read_values[0] = old;
        }
    }

    void _post_process()
    {
        __post_process_a(_line_a_prev, _is_needed_field(), _noise_level);
        __post_process_b(_is_upper_field_line() ? _line_b_prev : _line_c_prev, _noise_level);

        __post_process_truncate(_line_a_prev.Line(), _param.bitdepth_real, 10);
        __post_process_truncate(_line_b_prev.Line(), _param.bitdepth_real, 10);
        __post_process_truncate(_line_c_prev.Line(), _param.bitdepth_real, 10);

        _noise_level = __estimate_noise_level(_param.noise_level_1, _param.noise_level_2,
                                              _noise_less_than_distance_count,
                                              noise_levels);
    }
    uint32_t noise_levels[8];

    int _is_needed_field()
    {
        uint32_t a = 1;
        if (_param.bayer_pattern != 3)
            a = _param.bayer_pattern == 0;
        return ((_current_line_index & 1) != a) ? 0 : 1;
    }

    bool _is_upper_field_line()
    {
        return (_current_line_index & 1) == 0;
    }

    void _processComponentPair()
    {
        int default_lsb_size = _parameters_vl_mode.f;

        int a = _getValueBitSizeMinus1_ButMax6(_context_a.g);
        int b = _getValueBitSizeMinus1_ButMax6(_context_b.g);

        // Pre-read data
        uint64_t data = _reader().ShowBits(48);

        // Read component A most significant bits
        uint32_t size_in_bits_of_msb_a = 0;
        uint32_t msb_a = _readHuffmanValue(data, size_in_bits_of_msb_a);
        data >>= size_in_bits_of_msb_a;

        // Read component B most significant bits
        uint32_t size_in_bits_of_msb_b = 0;
        uint32_t msb_b = _readHuffmanValue(data, size_in_bits_of_msb_b);
        data >>= size_in_bits_of_msb_b;

        // Read component A least significant bits
        uint32_t lsb_a_size = msb_a == 12 ? default_lsb_size : a;
        uint32_t lsb_a = 0;
        if (lsb_a_size > 0)
        {
            lsb_a = ((data << (64 - lsb_a_size)) >> (64 - lsb_a_size));
            data >>= lsb_a_size;
        }

        // Read component B least significant bits
        uint32_t lsb_b_size = msb_b == 12 ? default_lsb_size : b;
        uint32_t lsb_b = 0;
        if (lsb_b_size > 0)
        {
            lsb_b = ((data << (64 - lsb_b_size)) >> (64 - lsb_b_size));
            data >>= lsb_b_size;
        }

        _reader().FlushBits(size_in_bits_of_msb_a + size_in_bits_of_msb_b + lsb_a_size + lsb_b_size);

        // Construct component values
        int value_a = msb_a == 12 ? lsb_a + 1 : (msb_a << lsb_a_size) | lsb_a;
        int value_b = msb_b == 12 ? lsb_b + 1 : (msb_b << lsb_b_size) | lsb_b;

        // Predict offsets
        auto predicted_offset_a = _fixPrediction(
            _context_a.last_new_read_values[1],
            _context_a.last_old_read_values[0],
            _context_a.last_old_read_values[1]);
        auto predicted_offset_b = _fixPrediction(
            _context_b.last_new_read_values[1],
            _context_b.last_old_read_values[0],
            _context_b.last_old_read_values[1]);

        // Get two's complement from constructed A value according to sign bit
        // Based on: https://en.wikipedia.org/wiki/Two%27s_complement
        bool sign_a = value_a & 1;
        int body_a = (value_a + 1) >> 1;
        int complement_a = sign_a ? -body_a : body_a;
        bool sign_b = value_b & 1;
        int body_b = (value_b + 1) >> 1;
        int complement_b = sign_b ? -body_b : body_b;

        // Fix component A value
        int pixel_value = _unmodValue(
            _parameters_vl_mode.b * complement_a + predicted_offset_a,
            _parameters_vl_mode.d, _param.max_allowed_pixel_value,
            _parameters_vl_mode.c, _parameters_vl_mode.b);

        // Round pixel value to [0; max_allowed_pixel_value)
        pixel_value = _roundValue(0, pixel_value, _param.max_allowed_pixel_value);

        // Save new read pixel value
        _context_a.last_new_read_values[0] = pixel_value;

        // ===

        if (value_a >> a > 11)
            --value_a;
        value_a = _roundValue(0, value_a, _param.max_allowed_raw_value);

        // Calculate block context next value
        _context_a.g = (2 * value_a + 2 * _context_a.g + 2) / 4;

        // ===

        // Fix component B value
        pixel_value = _unmodValue(
            _parameters_vl_mode.b * complement_b + predicted_offset_b,
            _parameters_vl_mode.d, _param.max_allowed_pixel_value,
            _parameters_vl_mode.c, _parameters_vl_mode.b);

        // Round pixel value to [0; max_allowed_pixel_value)
        pixel_value = _roundValue(0, pixel_value, _param.max_allowed_pixel_value);

        // Save new read pixel value
        _context_b.last_new_read_values[0] = pixel_value;

        // ===

        if (value_b >> b > 11)
            --value_b;
        value_b = _roundValue(0, value_b, _param.max_allowed_raw_value);

        // Calculate block context next value
        _context_b.g = (2 * value_b + 2 * _context_b.g + 2) / 4;
    }

    int _roundValue(int left, int value, int right)
    {
        if (value > right)
            value = right;
        if (value < left)
            value = left;
        return value;
    }

    int _getValueBitSizeMinus1_ButMax6(int value)
    {
        int i = 0;
        for (; i < 6; ++i)
            if (!(value >> (i + 1)))
                break;
        return i;
    }

    uint32_t _readHuffmanValue(uint32_t data_in, uint32_t& size_in_bits_out)
    {
        int i = 0;
        for (; i < 9; ++i)
        {
            if (data_in & 1)
                break;
            data_in >>= 1;
        }

        switch (i)
        {
        case 0:
            size_in_bits_out = 1;
            return 0;

        case 1:
            size_in_bits_out = 2;
            return 1;

        case 2:
            size_in_bits_out = 3;
            return 2;

        case 3:
            size_in_bits_out = 4;
            return 3;

        case 4:
            size_in_bits_out = 5;
            return 4;

        case 5:
            size_in_bits_out = 7;
            return ((data_in & 3) == 1 ? 5 : 6);

        case 6:
            size_in_bits_out = 8;
            return ((data_in & 3) == 1 ? 7 : 8);

        case 7:
            size_in_bits_out = 9;
            return ((data_in & 3) == 1 ? 11 : 12);

        case 8:
            size_in_bits_out = 9;
            return 10;

        default:
            break;
        }

        // default (000000000)
        size_in_bits_out = 9;
        return 9;
    }

    int _fixPrediction(int p1, int p2, int value)
    {
        // This function inverts value in local interval:
        // [a-------b]---value => result = a
        // value---[a-------b] => result = b
        // ================================================
        // Or standard case:
        // [a---------value--b] => result = a + b - value:
        // [a--result--------b] (offsets from a and b are exchanged)
        // ================================================
        // More simple formula for standard case (when result is not a and not b):
        // result = ((-(value - a)) mod (b-a)) + a

        int a = p1 <= p2 ? p1 : p2;
        int b = p1 <= p2 ? p2 : p1;

        if (b <= value)
            return a;

        if (a < value)
            return a + b - value;

        return b;
    }

    int _unmodValue(int value, int step, int safe_offset, int range, int step_count)
    {
        int result = value;

        if (value < -range)
            result = value + step_count * step;

        if (range + safe_offset < value)
            result = value - step_count * step;

        return result;
    }

    void _initBlockParameters()
    {
        int max_value = 1 << _bitdepth_diff;

        int v9 = (max_value >> 1) - 1;
        if (v9 < 0)
            v9 = 0;

        _parameters_vl_mode.a = _bitdepth_diff;
        _parameters_vl_mode.b = max_value;
        _parameters_vl_mode.c = v9;
        _parameters_vl_mode.d = ((2 * _parameters_vl_mode.c + _param.max_allowed_pixel_value) >> _parameters_vl_mode.a) + 1;
        _parameters_vl_mode.e = -floor(-log(_parameters_vl_mode.d) / log(2.0));
        _parameters_vl_mode.f = _param.bitdepth_real - _parameters_vl_mode.a;

        // Parameters for raw block reading mode are same
        _parameters_raw_mode = _parameters_vl_mode;
    }

    struct __decoding_context
    {
        int g;
        int last_new_read_values[3];
        int last_old_read_values[3];
    } _context_a, _context_b;

    void _collectNoiseLevelStatistics(__decoding_context &ctx, int distance)
    {
        // Get distance between 2 last pixel values
        int l1 = ctx.last_new_read_values[0] - ctx.last_new_read_values[1];
        l1 = l1 < 0 ? -l1 : l1;

        // Get distance between last pixel value and prev lines prev pixel value
        int l2 = ctx.last_old_read_values[1] - ctx.last_new_read_values[0];
        l2 = l2 < 0 ? -l2 : l2;

        // Use least distance
        int l12 = l1 <= l2 ? l1 : l2;

        // Get distance between last pixel values from different lines
        int l3 = ctx.last_old_read_values[0] - ctx.last_new_read_values[0];
        l3 = l3 < 0 ? -l3 : l3;

        // Use least distance
        int l123 = l12 <= l3 ? l12 : l3;

        // If distance is less than parameter - increase counter
        if (l123 < distance)
            ++_noise_less_than_distance_count;
    }

    // Parameters for variable-length and raw reading modes
    struct __block_parameters
    {
        uint32_t a;
        uint32_t b;
        uint32_t c;
        uint32_t d;
        int32_t e;
        uint32_t f;
    } _parameters_raw_mode, _parameters_vl_mode;

    ZRawImageBlockLine _line_a;
    ZRawImageBlockLine _line_bc;

    ZRawImageBlockLine _line_a_prev;
    ZRawImageBlockLine _line_b_prev;
    ZRawImageBlockLine _line_c_prev;

    int _read_values_count;
    int _current_block_index;
    int _current_line_index;

    uint64_t _decoding_mode;
    int _bitdepth_diff;

    uint32_t _noise_level;
    uint32_t _noise_less_than_distance_count;

    Parameters _param;
    BitReader *__reader;
};
