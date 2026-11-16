/* Start the VoiceMMode1 hostless path (PCM 2) without the audio HAL, hold it
 * for N seconds and stop. Mixer routing must be set beforehand (tinymix). */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <tinyalsa/asoundlib.h>

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 5;
    int dev = argc > 2 ? atoi(argv[2]) : 2;
    struct pcm_config c = { .channels = 1, .rate = 8000, .period_size = 160,
                            .period_count = 2, .format = PCM_FORMAT_S16_LE };
    struct pcm_config c2 = c;
    struct pcm *tx = pcm_open(0, dev, PCM_IN, &c);
    if (!pcm_is_ready(tx)) { printf("tx: %s\n", pcm_get_error(tx)); return 1; }
    struct pcm *rx = pcm_open(0, dev, PCM_OUT, &c2);
    if (!pcm_is_ready(rx)) { printf("rx: %s\n", pcm_get_error(rx)); return 1; }
    printf("start tx %d\n", pcm_start(tx));
    printf("start rx %d\n", pcm_start(rx));
    sleep(secs);
    pcm_close(rx);
    pcm_close(tx);
    return 0;
}
