// Host check for main/audio/ogg_opus.c: encode 3 s of a 440 Hz tone with libopus
// (16 kHz mono, 20 ms, 24 kbps, like the device), mux it, then validate with
// opusinfo/opusdec. Run: test/run_host_tests.sh
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <opus.h>
#include "../main/audio/ogg_opus.h"

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "test.ogg";
    int err;
    OpusEncoder *enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    assert(err == OPUS_OK);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
    opus_int32 lookahead;
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));

    FILE *f = fopen(out, "wb");
    assert(f);
    ogg_opus_t o;
    const char *tags[] = {"DEVICE_ID=nt-test", "SEQ=1"};
    assert(ogg_opus_begin(&o, f, 0x1234, 16000, (uint16_t)(lookahead * 3), "test", tags, 2) == 0);

    int16_t pcm[320];
    uint8_t pkt[1500];
    for (int frame = 0; frame < 150; frame++) {            // 150 x 20 ms = 3 s
        for (int i = 0; i < 320; i++)
            pcm[i] = (int16_t)(8000 * sin(2 * M_PI * 440 * (frame * 320 + i) / 16000.0));
        int n = opus_encode(enc, pcm, 320, pkt, sizeof(pkt));
        assert(n > 0);
        assert(ogg_opus_write_packet(&o, pkt, n, 960) == 0);
        if (frame == 74) assert(ogg_opus_flush(&o) == 0);   // mid-stream flush, as every 10 s on device
    }
    assert(ogg_opus_end(&o) == 0);
    assert(o.granule == 150 * 960);
    fclose(f);
    opus_encoder_destroy(enc);
    printf("wrote %s (%llu bytes)\n", out, (unsigned long long)o.bytes_written);
    return 0;
}
