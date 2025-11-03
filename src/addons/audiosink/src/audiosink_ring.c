#include "audiosink.h"
#include "common.h"
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

int au_ring_map_from_fd(audiosink_t *s, int fd){
    au_ring_close(s);

    const size_t PROBE = 65536;
    phau_hdr_t *probe = mmap(NULL, PROBE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(probe == MAP_FAILED){
        fprintf(stderr,"[audiosink] mmap probe: %s\n", strerror(errno));
        close(fd); return -1;
    }
    if(probe->magic != PHAU_MAGIC){
        fprintf(stderr,"[audiosink] bad magic in audio ring\n");
        munmap(probe, PROBE); close(fd); return -1;
    }

    size_t need = sizeof(phau_hdr_t) + probe->capacity;
    munmap(probe, PROBE);

    phau_hdr_t *full = mmap(NULL, need, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(full == MAP_FAILED){
        fprintf(stderr,"[audiosink] mmap full: %s\n", strerror(errno));
        close(fd); return -1;
    }

    s->memfd    = fd;
    s->hdr      = full;
    s->map_bytes= need;

    /* align consumer cursor to current producer write pos for "live" playback */
    atomic_store(&s->hdr->rpos, atomic_load(&s->hdr->wpos));

    fprintf(stderr,"[audiosink] ring mapped cap=%u fmt=%u rate=%.1f ch=%u\n",
            s->hdr->capacity, s->hdr->fmt, s->hdr->sample_rate, s->hdr->channels);
    return 0;
}

void au_ring_close(audiosink_t *s){
    if(s->hdr && s->hdr != MAP_FAILED) munmap(s->hdr, s->map_bytes);
    if(s->memfd >= 0) close(s->memfd);
    s->hdr = NULL; s->map_bytes=0; s->memfd=-1;
}

size_t au_ring_pop_f32(audiosink_t *s, float *dst, size_t max_frames){
    phau_hdr_t *h = s->hdr;
    if(!h) return 0;

    const size_t frame_bytes = (size_t)h->bytes_per_samp * (size_t)h->channels;
    uint64_t w = atomic_load(&h->wpos);
    uint64_t r = atomic_load(&h->rpos);
    if(w <= r) return 0;

    size_t avail_bytes  = (size_t)(w - r);
    size_t avail_frames = avail_bytes / frame_bytes;
    if(avail_frames == 0) return 0;
    if(avail_frames > max_frames) avail_frames = max_frames;

    size_t bytes = avail_frames * frame_bytes;
    size_t cap   = h->capacity;
    size_t rp    = (size_t)(r % cap);

    size_t first = bytes;
    if(rp + bytes > cap) first = cap - rp;

    memcpy(dst, h->data + rp, first);
    if(bytes > first) memcpy(((uint8_t*)dst)+first, h->data, bytes-first);

    atomic_store(&h->rpos, r + bytes);
    return avail_frames;
}
