#include "audiosink.h"
#include <alloca.h>  
#include <stdio.h>

void au_pcm_close(audiosink_t *s){
    if(s->pcm){
        snd_pcm_drain(s->pcm);
        snd_pcm_close(s->pcm);
        s->pcm = NULL;
    }
}

int au_pcm_open(audiosink_t *s, unsigned rate, unsigned ch){
    if(s->pcm) au_pcm_close(s);

    snd_pcm_t *pcm = NULL;
    int rc = snd_pcm_open(&pcm, s->alsa_dev[0] ? s->alsa_dev : "default",
                          SND_PCM_STREAM_PLAYBACK, 0);
    if(rc < 0){
        fprintf(stderr,"[audiosink] snd_pcm_open(%s): %s\n",
                s->alsa_dev[0]?s->alsa_dev:"default", snd_strerror(rc));
        return -1;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_FLOAT_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, ch);

    unsigned rr = rate;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rr, 0);

    snd_pcm_uframes_t period = 480;  /* ~10 ms @ 48k */
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0);
    snd_pcm_uframes_t bufsize = period * 4;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufsize);

    rc = snd_pcm_hw_params(pcm, hw);
    if(rc < 0){
        fprintf(stderr,"[audiosink] hw_params: %s\n", snd_strerror(rc));
        snd_pcm_close(pcm);
        return -1;
    }

    s->pcm      = pcm;
    s->pcm_rate = rr;
    s->pcm_ch   = ch;

    fprintf(stderr,"[audiosink] ALSA ready dev=%s rate=%u ch=%u period=%lu buf=%lu\n",
            s->alsa_dev[0]?s->alsa_dev:"default", s->pcm_rate, s->pcm_ch,
            (unsigned long)period, (unsigned long)bufsize);
    return 0;
}
