/* Emulate an ATAPI CD-ROM drive reading a disc image. */
#include "core/atapi.h"

#include <string.h>

#define STATUS_BUSY   0x80u
#define STATUS_READY  0x40u
#define STATUS_SEEK   0x10u
#define STATUS_DRQ    0x08u
#define STATUS_ERROR  0x01u
#define STATUS_IDLE   (STATUS_READY | STATUS_SEEK)

#define ERROR_ABORT   0x04u
#define REASON_COMMAND 0x01u
#define REASON_IN     0x02u
#define FEATURE_DMA   0x01u
#define HEAD_DEVICE_1 0x10u
#define CONTROL_NIEN  0x02u
#define CONTROL_SRST  0x04u

#define ATA_DEVICE_RESET     0x08u
#define ATA_PACKET           0xA0u
#define ATA_IDENTIFY_PACKET  0xA1u
#define ATA_CHECK_POWER_MODE 0xE5u
#define ATA_IDENTIFY         0xECu
#define ATA_SET_FEATURES     0xEFu
#define ATA_IDLE_IMMEDIATE   0xE1u
#define ATA_STANDBY_IMMEDIATE 0xE0u
#define ATA_NOP              0x00u

#define SCSI_TEST_UNIT_READY 0x00u
#define SCSI_REQUEST_SENSE   0x03u
#define SCSI_INQUIRY         0x12u
#define SCSI_MODE_SELECT_6   0x15u
#define SCSI_MODE_SENSE_6    0x1Au
#define SCSI_START_STOP      0x1Bu
#define SCSI_PREVENT_REMOVAL 0x1Eu
#define SCSI_READ_CAPACITY   0x25u
#define SCSI_READ_10         0x28u
#define SCSI_SEEK_10         0x2Bu
#define SCSI_SYNCHRONIZE     0x35u
#define SCSI_READ_SUBCHANNEL 0x42u
#define SCSI_READ_TOC        0x43u
#define SCSI_PLAY_AUDIO_10   0x45u
#define SCSI_PLAY_AUDIO_MSF  0x47u
#define SCSI_PAUSE_RESUME    0x4Bu
#define SCSI_STOP_PLAY       0x4Eu
#define SCSI_MODE_SELECT_10  0x55u
#define SCSI_MODE_SENSE_10   0x5Au
#define SCSI_READ_12         0xA8u
#define SCSI_SET_SPEED       0xBBu
#define SCSI_MECHANISM_STATUS 0xBDu
#define SCSI_READ_CD         0xBEu

#define SENSE_NOT_READY       0x02u
#define SENSE_MEDIUM_ERROR    0x03u
#define SENSE_ILLEGAL_REQUEST 0x05u
#define SENSE_UNIT_ATTENTION  0x06u
#define ASC_READ_ERROR        0x11u
#define ASC_INVALID_OPCODE    0x20u
#define ASC_OUT_OF_RANGE      0x21u
#define ASC_INVALID_FIELD     0x24u
#define ASC_MEDIUM_CHANGED    0x28u
#define ASC_NO_MEDIUM         0x3Au
#define ASC_ILLEGAL_MODE      0x64u

#define TRACK_DATA        0x14u
#define TRACK_LEAD_OUT    0xAAu
#define FRAMES_PER_SECOND 75u
#define LEAD_IN_FRAMES    150u
#define MAX_TRANSFER      0xFFFEu
#define READ_CD_USER_DATA 0x10u
#define READ_CD_TYPE_ANY  0u
#define READ_CD_TYPE_AUDIO 1u
#define READ_CD_TYPE_MODE1 2u
#define TRACK_ADR         0x10u

static void clear_transfer(atapi_t *drive) {
    drive->buffer_length = drive->buffer_position = drive->chunk_left = 0;
    drive->blocks_left = 0;
    drive->writing_packet = false;
    drive->dma = false;
}

static void set_signature(atapi_t *drive) {
    drive->sector_count = 1;
    drive->sector_number = 1;
    drive->byte_count_low = 0x14;
    drive->byte_count_high = 0xEB;
    drive->drive_head &= HEAD_DEVICE_1;
}

static void power_on_state(atapi_t *drive) {
    clear_transfer(drive);
    drive->irq = false;
    drive->error = 0x01;
    drive->status = 0;
    drive->drive_head = 0;
    set_signature(drive);
}

void atapi_reset(atapi_slot_t *slot) {
    atapi_t *drive = slot->state;
    drive->device_control = 0;
    drive->sense_key = drive->sense_code = drive->sense_qualifier = 0;
    power_on_state(drive);
}

bool atapi_load(atapi_slot_t *slot, FILE *image, const cdimage_t *disc) {
    if (!image) return false;
    if (slot->image) fclose(slot->image);
    slot->image = image;
    slot->disc = *disc;
    slot->ready_at = 0;
    slot->state->total_blocks = disc->total_blocks;
    slot->state->loaded = true;
    slot->state->media_changed = true;
    return true;
}

void atapi_unload(atapi_slot_t *slot) {
    if (slot->image) fclose(slot->image);
    slot->image = NULL;
    slot->state->loaded = false;
    slot->state->media_changed = true;
    slot->state->total_blocks = 0;
    memset(&slot->disc, 0, sizeof slot->disc);
}

void atapi_rebind(atapi_slot_t *slot, FILE *image, const cdimage_t *disc) {
    if (slot->image && slot->image != image) fclose(slot->image);
    slot->image = image;
    slot->disc = *disc;
    slot->ready_at = 0;
    if (slot->state->loaded && !image) {
        slot->state->loaded = false;
        slot->state->media_changed = true;
        clear_transfer(slot->state);
    }
}

void atapi_sanitize(atapi_t *drive) {
    bool valid = drive->buffer_length <= sizeof drive->buffer && drive->buffer_position <= drive->buffer_length &&
                 drive->chunk_left <= drive->buffer_length - drive->buffer_position && drive->packet_position <= sizeof drive->packet;
    if (valid) return;
    drive->status &= (uint8_t) ~(STATUS_DRQ | STATUS_BUSY);
    clear_transfer(drive);
}

static bool device_1(const atapi_t *drive) {
    return (drive->drive_head & HEAD_DEVICE_1) != 0;
}

static void raise_interrupt(atapi_t *drive) {
    drive->irq = true;
}

static void complete(atapi_t *drive) {
    clear_transfer(drive);
    drive->error = 0;
    drive->status = STATUS_IDLE;
    drive->sector_count = REASON_COMMAND | REASON_IN;
    raise_interrupt(drive);
}

static void check_condition(atapi_t *drive, uint8_t key, uint8_t code, uint8_t qualifier) {
    clear_transfer(drive);
    drive->sense_key = key;
    drive->sense_code = code;
    drive->sense_qualifier = qualifier;
    drive->error = (uint8_t)(key << 4) | ERROR_ABORT;
    drive->status = STATUS_IDLE | STATUS_ERROR;
    drive->sector_count = REASON_COMMAND | REASON_IN;
    raise_interrupt(drive);
}

static void abort_command(atapi_t *drive) {
    drive->error = ERROR_ABORT;
    drive->status = STATUS_IDLE | STATUS_ERROR;
    raise_interrupt(drive);
}

static void start_chunk(atapi_t *drive) {
    uint32_t limit = (uint32_t)drive->byte_count_high << 8 | drive->byte_count_low;
    if (limit == 0 || limit > MAX_TRANSFER) limit = MAX_TRANSFER;
    limit &= ~1u;
    uint32_t chunk = drive->buffer_length - drive->buffer_position;
    if (chunk > limit) chunk = limit;
    drive->chunk_left = chunk;
    drive->byte_count_low = (uint8_t)chunk;
    drive->byte_count_high = (uint8_t)(chunk >> 8);
    drive->sector_count = REASON_IN;
    drive->status = STATUS_IDLE | STATUS_DRQ;
    raise_interrupt(drive);
}

static void begin_data_in(atapi_t *drive) {
    if (drive->buffer_length == 0) {
        complete(drive);
        return;
    }
    drive->buffer_position = 0;
    if (drive->dma) {
        drive->sector_count = REASON_IN;
        drive->status = STATUS_IDLE | STATUS_DRQ;
        return;
    }
    start_chunk(drive);
}

static void respond(atapi_t *drive, const uint8_t *data, uint32_t length, uint32_t allocation) {
    if (length > allocation) length = allocation;
    if (length > sizeof drive->buffer) length = sizeof drive->buffer;
    memcpy(drive->buffer, data, length);
    drive->buffer_length = length;
    begin_data_in(drive);
}

static uint64_t clock_now(const atapi_slot_t *slot) {
    return slot->clock ? slot->clock(slot->clock_context) : 0;
}

static bool fill_blocks(atapi_slot_t *slot) {
    atapi_t *drive = slot->state;
    uint32_t block_bytes = drive->audio ? CDIMAGE_RAW_BYTES : CDIMAGE_DATA_BYTES;
    uint32_t room = (uint32_t)sizeof drive->buffer / block_bytes;
    uint32_t count = drive->blocks_left < room ? drive->blocks_left : room;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *out = drive->buffer + i * block_bytes;
        bool ok = drive->audio ? cdimage_read_audio(slot->image, &slot->disc, drive->next_block + i, out)
                               : cdimage_read_data(slot->image, &slot->disc, drive->next_block + i, out);
        if (!ok) return false;
    }
    drive->current_block = drive->next_block + count - 1;
    drive->next_block += count;
    drive->blocks_left -= count;
    drive->buffer_length = count * block_bytes;
    drive->buffer_position = 0;
    if (drive->audio && slot->clock_hz) slot->ready_at = clock_now(slot) + (uint64_t)count * slot->clock_hz / FRAMES_PER_SECOND;
    return true;
}

static void start_read(atapi_slot_t *slot, uint32_t block, uint32_t count, uint32_t type) {
    atapi_t *drive = slot->state;
    if (count == 0) {
        complete(drive);
        return;
    }
    if ((uint64_t)block + count > drive->total_blocks) {
        check_condition(drive, SENSE_ILLEGAL_REQUEST, ASC_OUT_OF_RANGE, 0);
        return;
    }
    const cdimage_track_t *track = cdimage_track_at(&slot->disc, block);
    bool audio = track && !(track->control & CDIMAGE_CONTROL_DATA);
    if (!track || (type == READ_CD_TYPE_AUDIO && !audio) || (type == READ_CD_TYPE_MODE1 && audio)) {
        check_condition(drive, SENSE_ILLEGAL_REQUEST, ASC_ILLEGAL_MODE, 0);
        return;
    }
    bool dma = drive->dma;
    clear_transfer(drive);
    drive->dma = dma;
    drive->audio = audio;
    drive->next_block = block;
    drive->blocks_left = count;
    if (!fill_blocks(slot)) {
        check_condition(drive, SENSE_MEDIUM_ERROR, ASC_READ_ERROR, 0);
        return;
    }
    begin_data_in(drive);
}

static void buffer_drained(atapi_slot_t *slot) {
    atapi_t *drive = slot->state;
    if (drive->buffer_position < drive->buffer_length) {
        if (!drive->dma) start_chunk(drive);
        return;
    }
    if (drive->blocks_left == 0) {
        complete(drive);
        return;
    }
    if (!fill_blocks(slot)) {
        check_condition(drive, SENSE_MEDIUM_ERROR, ASC_READ_ERROR, 0);
        return;
    }
    if (!drive->dma) start_chunk(drive);
}

static uint32_t read_be16(const uint8_t *bytes) {
    return (uint32_t)bytes[0] << 8 | bytes[1];
}

static uint32_t read_be24(const uint8_t *bytes) {
    return (uint32_t)bytes[0] << 16 | read_be16(bytes + 1);
}

static uint32_t read_be32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] << 24 | read_be24(bytes + 1);
}

static void write_be16(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void write_be32(uint8_t *bytes, uint32_t value) {
    write_be16(bytes, value >> 16);
    write_be16(bytes + 2, value);
}

static void write_address(uint8_t *bytes, uint32_t block, bool msf) {
    if (!msf) {
        write_be32(bytes, block);
        return;
    }
    uint32_t frames = block + LEAD_IN_FRAMES;
    bytes[0] = 0;
    bytes[1] = (uint8_t)(frames / (60 * FRAMES_PER_SECOND));
    bytes[2] = (uint8_t)(frames / FRAMES_PER_SECOND % 60);
    bytes[3] = (uint8_t)(frames % FRAMES_PER_SECOND);
}

static void put_ata_string(uint8_t *destination, const char *text, size_t bytes) {
    size_t length = strlen(text);
    for (size_t i = 0; i < bytes; i++) destination[i ^ 1] = i < length ? (uint8_t)text[i] : ' ';
}

static void identify_packet(atapi_t *drive) {
    uint16_t words[256] = { 0 };
    words[0] = 0x8580;
    words[49] = 0x0300;
    words[53] = 0x0006;
    words[63] = 0x0007;
    words[64] = 0x0003;
    words[65] = words[66] = words[67] = words[68] = 120;
    words[80] = 0x001E;
    uint8_t *bytes = drive->buffer;
    for (int i = 0; i < 256; i++) {
        bytes[2 * i] = (uint8_t)words[i];
        bytes[2 * i + 1] = (uint8_t)(words[i] >> 8);
    }
    put_ata_string(bytes + 20, "SH3EMU0001", 20);
    put_ata_string(bytes + 46, "1.00", 8);
    put_ata_string(bytes + 54, "SH3EMU CD-ROM", 40);
    drive->buffer_length = 512;
    drive->byte_count_low = 0;
    drive->byte_count_high = 2;
    drive->dma = false;
    begin_data_in(drive);
}

static void inquiry(atapi_t *drive, uint32_t allocation) {
    uint8_t data[36] = { 0x05, 0x80, 0x00, 0x21, 31 };
    memcpy(data + 8, "SH3EMU  ", 8);
    memcpy(data + 16, "CD-ROM          ", 16);
    memcpy(data + 32, "1.00", 4);
    respond(drive, data, sizeof data, allocation);
}

static void request_sense(atapi_t *drive, uint32_t allocation) {
    uint8_t data[18] = { 0x70, 0, drive->sense_key, 0, 0, 0, 0, 10 };
    data[12] = drive->sense_code;
    data[13] = drive->sense_qualifier;
    drive->sense_key = drive->sense_code = drive->sense_qualifier = 0;
    respond(drive, data, sizeof data, allocation);
}

static void read_capacity(atapi_t *drive) {
    uint8_t data[8];
    write_be32(data, drive->total_blocks ? drive->total_blocks - 1 : 0);
    write_be32(data + 4, CDIMAGE_DATA_BYTES);
    respond(drive, data, sizeof data, sizeof data);
}

static uint8_t track_adr_control(const cdimage_track_t *track) {
    return (uint8_t)(TRACK_ADR | track->control);
}

static void full_toc(atapi_slot_t *slot, uint32_t allocation) {
    const cdimage_t *disc = &slot->disc;
    uint8_t data[4 + 11 * (3 + CDIMAGE_TRACKS_MAX)] = { 0 };
    const cdimage_track_t *first = &disc->tracks[0], *last = &disc->tracks[disc->track_count - 1];
    uint8_t *entry = data + 4;
    for (int point = 0; point < 3; point++, entry += 11) {
        entry[0] = 1;
        entry[1] = track_adr_control(point == 0 ? first : last);
        entry[3] = (uint8_t)(0xA0 + point);
        if (point < 2) entry[8] = point == 0 ? first->number : last->number;
        else write_address(entry + 7, disc->total_blocks, true);
    }
    for (uint32_t i = 0; i < disc->track_count; i++, entry += 11) {
        entry[0] = 1;
        entry[1] = track_adr_control(&disc->tracks[i]);
        entry[3] = disc->tracks[i].number;
        write_address(entry + 7, disc->tracks[i].start, true);
    }
    uint32_t length = (uint32_t)(entry - data);
    write_be16(data, length - 2);
    data[2] = data[3] = 1;
    respond(slot->state, data, length, allocation);
}

static void read_toc(atapi_slot_t *slot, const uint8_t *packet) {
    atapi_t *drive = slot->state;
    const cdimage_t *disc = &slot->disc;
    bool msf = (packet[1] & 0x02) != 0;
    uint32_t format = packet[2] & 0x0F;
    if (format == 0 && (packet[9] >> 6)) format = packet[9] >> 6;
    uint32_t allocation = read_be16(packet + 7);
    uint8_t data[4 + 8 * (1 + CDIMAGE_TRACKS_MAX)] = { 0 };
    uint8_t *entry = data + 4;
    if (format == 0) {
        uint32_t start = packet[6];
        if (start > disc->tracks[disc->track_count - 1].number && start != TRACK_LEAD_OUT) {
            check_condition(drive, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
            return;
        }
        for (uint32_t i = 0; i < disc->track_count; i++) {
            if (disc->tracks[i].number < start) continue;
            entry[1] = track_adr_control(&disc->tracks[i]);
            entry[2] = disc->tracks[i].number;
            write_address(entry + 4, disc->tracks[i].start, msf);
            entry += 8;
        }
        entry[1] = track_adr_control(&disc->tracks[disc->track_count - 1]);
        entry[2] = TRACK_LEAD_OUT;
        write_address(entry + 4, disc->total_blocks, msf);
        entry += 8;
        data[2] = disc->tracks[0].number;
        data[3] = disc->tracks[disc->track_count - 1].number;
    } else if (format == 1) {
        entry[1] = track_adr_control(&disc->tracks[0]);
        entry[2] = disc->tracks[0].number;
        write_address(entry + 4, disc->tracks[0].start, msf);
        entry += 8;
        data[2] = data[3] = 1;
    } else if (format == 2) {
        full_toc(slot, allocation);
        return;
    } else {
        check_condition(drive, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        return;
    }
    uint32_t length = (uint32_t)(entry - data);
    write_be16(data, length - 2);
    respond(drive, data, length, allocation);
}

static void read_subchannel(atapi_slot_t *slot, const uint8_t *packet) {
    atapi_t *drive = slot->state;
    uint8_t data[16] = { 0, 0x15 };
    uint32_t length = 4;
    if (packet[2] & 0x40) {
        bool msf = (packet[1] & 0x02) != 0;
        uint32_t block = drive->current_block;
        const cdimage_track_t *track = cdimage_track_at(&slot->disc, block);
        data[3] = 12;
        data[4] = packet[3] ? packet[3] : 1;
        data[5] = track ? track_adr_control(track) : TRACK_DATA;
        data[6] = track ? track->number : 1;
        data[7] = 1;
        write_address(data + 8, block, msf);
        write_address(data + 12, track ? block - track->start : 0, msf);
        length = 16;
    }
    respond(drive, data, length, read_be16(packet + 7));
}

static void mode_sense(atapi_t *drive, const uint8_t *packet, bool ten) {
    uint8_t page = packet[2] & 0x3F;
    uint8_t data[8 + 28] = { 0 };
    uint32_t header = ten ? 8 : 4;
    uint32_t length = header;
    if (page == 0x2A || page == 0x3F) {
        uint8_t *capabilities = data + header;
        capabilities[0] = 0x2A;
        capabilities[1] = 20;
        capabilities[2] = 0x03;
        capabilities[4] = 0x71;
        capabilities[6] = 0x29;
        write_be16(capabilities + 8, 706);
        write_be16(capabilities + 10, 256);
        write_be16(capabilities + 12, 64);
        write_be16(capabilities + 14, 706);
        length += 22;
    } else if (page != 0x0D && page != 0x0E && page != 0x01) {
        check_condition(drive, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        return;
    }
    if (ten) {
        write_be16(data, length - 2);
        data[2] = drive->loaded ? 0x01 : 0x70;
    } else {
        data[0] = (uint8_t)(length - 1);
        data[1] = drive->loaded ? 0x01 : 0x70;
    }
    respond(drive, data, length, ten ? read_be16(packet + 7) : packet[4]);
}

static void begin_data_out(atapi_t *drive, uint32_t length) {
    if (length == 0 || length > sizeof drive->buffer) {
        complete(drive);
        return;
    }
    drive->dma = false;
    drive->buffer_length = length;
    drive->buffer_position = 0;
    drive->chunk_left = length;
    drive->byte_count_low = (uint8_t)length;
    drive->byte_count_high = (uint8_t)(length >> 8);
    drive->sector_count = 0;
    drive->status = STATUS_IDLE | STATUS_DRQ;
    raise_interrupt(drive);
}

static bool needs_medium(uint8_t opcode) {
    switch (opcode) {
    case SCSI_TEST_UNIT_READY:
    case SCSI_READ_CAPACITY:
    case SCSI_READ_10:
    case SCSI_READ_12:
    case SCSI_READ_CD:
    case SCSI_READ_TOC:
    case SCSI_SEEK_10:
    case SCSI_PLAY_AUDIO_10:
    case SCSI_PLAY_AUDIO_MSF:
        return true;
    default:
        return false;
    }
}

static void execute_packet(atapi_slot_t *slot) {
    atapi_t *drive = slot->state;
    const uint8_t *packet = drive->packet;
    uint8_t opcode = packet[0];
    drive->writing_packet = false;
    if (drive->media_changed && opcode != SCSI_REQUEST_SENSE && opcode != SCSI_INQUIRY) {
        drive->media_changed = false;
        check_condition(drive, SENSE_UNIT_ATTENTION, ASC_MEDIUM_CHANGED, 0);
        return;
    }
    if (needs_medium(opcode) && !drive->loaded) {
        check_condition(drive, SENSE_NOT_READY, ASC_NO_MEDIUM, 0);
        return;
    }
    switch (opcode) {
    case SCSI_TEST_UNIT_READY:
    case SCSI_START_STOP:
    case SCSI_PREVENT_REMOVAL:
    case SCSI_SEEK_10:
    case SCSI_SYNCHRONIZE:
    case SCSI_PAUSE_RESUME:
    case SCSI_STOP_PLAY:
    case SCSI_SET_SPEED:
        complete(drive);
        break;
    case SCSI_REQUEST_SENSE:
        request_sense(drive, packet[4]);
        break;
    case SCSI_INQUIRY:
        inquiry(drive, packet[4]);
        break;
    case SCSI_READ_CAPACITY:
        read_capacity(drive);
        break;
    case SCSI_READ_10:
        start_read(slot, read_be32(packet + 2), read_be16(packet + 7), READ_CD_TYPE_MODE1);
        break;
    case SCSI_READ_12:
        start_read(slot, read_be32(packet + 2), read_be32(packet + 6), READ_CD_TYPE_MODE1);
        break;
    case SCSI_READ_CD:
        if ((packet[9] & 0xF8) != READ_CD_USER_DATA) check_condition(drive, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        else start_read(slot, read_be32(packet + 2), read_be24(packet + 6), (packet[1] >> 2) & 7u);
        break;
    case SCSI_READ_TOC:
        read_toc(slot, packet);
        break;
    case SCSI_READ_SUBCHANNEL:
        read_subchannel(slot, packet);
        break;
    case SCSI_MODE_SENSE_6:
        mode_sense(drive, packet, false);
        break;
    case SCSI_MODE_SENSE_10:
        mode_sense(drive, packet, true);
        break;
    case SCSI_MODE_SELECT_6:
        begin_data_out(drive, packet[4]);
        break;
    case SCSI_MODE_SELECT_10:
        begin_data_out(drive, read_be16(packet + 7));
        break;
    case SCSI_PLAY_AUDIO_10:
    case SCSI_PLAY_AUDIO_MSF:
        check_condition(drive, SENSE_ILLEGAL_REQUEST, ASC_ILLEGAL_MODE, 0);
        break;
    case SCSI_MECHANISM_STATUS: {
        uint8_t data[8] = { 0 };
        respond(drive, data, sizeof data, read_be16(packet + 8));
        break;
    }
    default:
        check_condition(drive, SENSE_ILLEGAL_REQUEST, ASC_INVALID_OPCODE, 0);
        break;
    }
}

static void execute_command(atapi_slot_t *slot, uint8_t command) {
    atapi_t *drive = slot->state;
    drive->irq = false;
    switch (command) {
    case ATA_DEVICE_RESET:
        clear_transfer(drive);
        set_signature(drive);
        drive->error = 0x01;
        drive->status = 0;
        break;
    case ATA_PACKET:
        clear_transfer(drive);
        drive->dma = (drive->feature & FEATURE_DMA) != 0;
        drive->writing_packet = true;
        drive->packet_position = 0;
        drive->sector_count = REASON_COMMAND;
        drive->status = STATUS_READY | STATUS_DRQ;
        break;
    case ATA_IDENTIFY_PACKET:
        clear_transfer(drive);
        identify_packet(drive);
        break;
    case ATA_IDENTIFY:
        set_signature(drive);
        abort_command(drive);
        break;
    case ATA_CHECK_POWER_MODE:
        drive->sector_count = 0xFF;
        drive->status = STATUS_IDLE;
        raise_interrupt(drive);
        break;
    case ATA_SET_FEATURES:
    case ATA_IDLE_IMMEDIATE:
    case ATA_STANDBY_IMMEDIATE:
        drive->status = STATUS_IDLE;
        raise_interrupt(drive);
        break;
    default:
        abort_command(drive);
        break;
    }
}

uint8_t atapi_read(atapi_slot_t *slot, int reg) {
    atapi_t *drive = slot->state;
    if (device_1(drive) && reg != ATAPI_DRIVE_HEAD) return 0;
    switch (reg) {
    case ATAPI_FEATURE: return drive->error;
    case ATAPI_SECTOR_COUNT: return drive->sector_count;
    case ATAPI_SECTOR_NUMBER: return drive->sector_number;
    case ATAPI_BYTE_COUNT_LOW: return drive->byte_count_low;
    case ATAPI_BYTE_COUNT_HIGH: return drive->byte_count_high;
    case ATAPI_DRIVE_HEAD: return drive->drive_head | 0xA0;
    case ATAPI_COMMAND:
        drive->irq = false;
        return drive->status;
    default: return 0xFF;
    }
}

void atapi_write(atapi_slot_t *slot, int reg, uint8_t value) {
    atapi_t *drive = slot->state;
    switch (reg) {
    case ATAPI_FEATURE: drive->feature = value; break;
    case ATAPI_SECTOR_COUNT: drive->sector_count = value; break;
    case ATAPI_SECTOR_NUMBER: drive->sector_number = value; break;
    case ATAPI_BYTE_COUNT_LOW: drive->byte_count_low = value; break;
    case ATAPI_BYTE_COUNT_HIGH: drive->byte_count_high = value; break;
    case ATAPI_DRIVE_HEAD: drive->drive_head = value; break;
    case ATAPI_COMMAND:
        if (!device_1(drive)) execute_command(slot, value);
        break;
    default: break;
    }
}

uint16_t atapi_read_data(atapi_slot_t *slot) {
    atapi_t *drive = slot->state;
    if (device_1(drive) || drive->dma || drive->writing_packet || drive->sector_count != REASON_IN || drive->chunk_left == 0) return 0;
    uint32_t bytes = drive->chunk_left < 2 ? drive->chunk_left : 2;
    uint16_t value = drive->buffer[drive->buffer_position];
    if (bytes == 2) value |= (uint16_t)(drive->buffer[drive->buffer_position + 1] << 8);
    drive->buffer_position += bytes;
    drive->chunk_left -= bytes;
    if (drive->chunk_left == 0) buffer_drained(slot);
    return value;
}

void atapi_write_data(atapi_slot_t *slot, uint16_t value) {
    atapi_t *drive = slot->state;
    if (device_1(drive)) return;
    if (drive->writing_packet) {
        if (drive->packet_position + 2 > sizeof drive->packet) return;
        drive->packet[drive->packet_position++] = (uint8_t)value;
        drive->packet[drive->packet_position++] = (uint8_t)(value >> 8);
        if (drive->packet_position == sizeof drive->packet) execute_packet(slot);
        return;
    }
    if (drive->sector_count != 0 || drive->chunk_left < 2) return;
    drive->buffer_position += 2;
    drive->chunk_left -= 2;
    if (drive->chunk_left == 0) complete(drive);
}

uint8_t atapi_alternate_status(const atapi_t *drive) {
    return device_1(drive) ? 0 : drive->status;
}

void atapi_write_control(atapi_slot_t *slot, uint8_t value) {
    atapi_t *drive = slot->state;
    bool resetting = (drive->device_control & CONTROL_SRST) && !(value & CONTROL_SRST);
    drive->device_control = value;
    if (value & CONTROL_SRST) {
        clear_transfer(drive);
        drive->status = STATUS_BUSY;
        drive->irq = false;
    } else if (resetting) {
        power_on_state(drive);
    }
}

bool atapi_interrupt(const atapi_t *drive) {
    return drive->irq && !(drive->device_control & CONTROL_NIEN);
}

static bool dma_pending(const atapi_t *drive) {
    return drive->dma && (drive->status & STATUS_DRQ) && drive->buffer_position < drive->buffer_length;
}

bool atapi_dma_ready(const atapi_slot_t *slot) {
    return dma_pending(slot->state) && (!slot->state->audio || clock_now(slot) >= slot->ready_at);
}

uint64_t atapi_dma_due(const atapi_slot_t *slot) {
    return dma_pending(slot->state) && slot->state->audio ? slot->ready_at : 0;
}

uint32_t atapi_dma_read(atapi_slot_t *slot, uint8_t *out, uint32_t length) {
    atapi_t *drive = slot->state;
    uint32_t copied = 0;
    while (copied < length && atapi_dma_ready(slot)) {
        uint32_t available = drive->buffer_length - drive->buffer_position;
        uint32_t take = length - copied < available ? length - copied : available;
        memcpy(out + copied, drive->buffer + drive->buffer_position, take);
        drive->buffer_position += take;
        copied += take;
        if (drive->buffer_position == drive->buffer_length) buffer_drained(slot);
    }
    return copied;
}
