
#include "ph_uds_protocol.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static void usage(void){
    fprintf(stderr, "ph-cli usage:\n");
    fprintf(stderr, "  ph-cli help\n");
    fprintf(stderr, "  ph-cli cmd \"<text>\"\n");
    fprintf(stderr, "  ph-cli pub <feed> \"<data>\"\n");
    fprintf(stderr, "  ph-cli sub <feed> [feed2 ...]\n");
    fprintf(stderr, "  ph-cli list feeds | list addons | available-addons\n");
    fprintf(stderr, "  ph-cli load addon <name|/path/to/lib.so>\n");
    fprintf(stderr, "  ph-cli unload addon <name>\n");
}

static int extract_feed(const char *js, char *out, size_t cap){
    const char *p = strstr(js, "\"feed\":\"");
    if(!p) return -1;
    p += 8;
    const char *q = strchr(p, '"');
    if(!q) return -1;
    size_t n = (size_t)(q - p);
    if(n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

int main(int argc, char **argv){
    if(argc<2){ usage(); return 1; }
    int fd = uds_connect(PH_SOCK_PATH);
    if(fd<0){ perror("connect"); return 1; }
    char buf[POC_MAX_JSON];

    if(strcmp(argv[1],"help")==0){
        usage(); close(fd); return 0;
    } else if(strcmp(argv[1],"cmd")==0 && argc>=3){
        snprintf(buf, sizeof buf, "{\"type\":\"command\",\"feed\":\"cli-control\",\"data\":\"%s\"}", argv[2]);
        send_frame_json(fd, buf, strlen(buf));
    } else if(strcmp(argv[1],"pub")==0 && argc>=4){
        snprintf(buf, sizeof buf, "{\"type\":\"publish\",\"feed\":\"%s\",\"data\":\"%s\",\"encoding\":\"utf8\"}", argv[2], argv[3]);
        send_frame_json(fd, buf, strlen(buf));
    } else if(strcmp(argv[1],"sub")==0 && argc>=3){
        for(int i=2;i<argc;i++){
            snprintf(buf, sizeof buf, "{\"type\":\"subscribe\",\"feed\":\"%s\"}", argv[i]);
            send_frame_json(fd, buf, strlen(buf));
        }
        fprintf(stderr, "[ph-cli] subscribed to %d feed(s). Ctrl+C to stop.\n", argc-2);
        while(1){
            char js[POC_MAX_JSON];
            int got = recv_frame_json(fd, js, sizeof js, 2000);
            if(got<=0) continue;
            char tag[128];
            if(extract_feed(js, tag, sizeof tag)==0)
                fprintf(stdout, "[%s] %s\n", tag, js);
            else
                fprintf(stdout, "%s\n", js);
            fflush(stdout);
        }
    } else if(strcmp(argv[1],"list")==0 && argc>=3){
        if(strcmp(argv[2],"feeds")==0) snprintf(buf,sizeof buf,"{\"type\":\"command\",\"feed\":\"cli-control\",\"data\":\"feeds\"}");
        else if(strcmp(argv[2],"addons")==0) snprintf(buf,sizeof buf,"{\"type\":\"command\",\"feed\":\"cli-control\",\"data\":\"plugins\"}");
        else if(strcmp(argv[2],"available-addons")==0) snprintf(buf,sizeof buf,"{\"type\":\"command\",\"feed\":\"cli-control\",\"data\":\"available-addons\"}");
        else { usage(); close(fd); return 1; }
        send_frame_json(fd, buf, strlen(buf));
    } else if(strcmp(argv[1],"available-addons")==0){
        snprintf(buf,sizeof buf,"{\"type\":\"command\",\"feed\":\"cli-control\",\"data\":\"available-addons\"}");
        send_frame_json(fd, buf, strlen(buf));
    } else if(strcmp(argv[1],"load")==0 && argc>=4 && strcmp(argv[2],"addon")==0){
        char joined[512]; joined[0]=0;
        for(int i=3;i<argc;i++){ if(i>3) strcat(joined," "); strcat(joined,argv[i]); }
        snprintf(buf,sizeof buf,"{\"type\":\"command\",\"feed\":\"cli-control\",\"data\":\"load %s\"}", joined);
        send_frame_json(fd, buf, strlen(buf));
    } else if(strcmp(argv[1],"unload")==0 && argc>=4 && strcmp(argv[2],"addon")==0){
        snprintf(buf,sizeof buf,"{\"type\":\"command\",\"feed\":\"cli-control\",\"data\":\"unload %s\"}", argv[3]);
        send_frame_json(fd, buf, strlen(buf));
    } else {
        usage(); close(fd); return 1;
    }

    // Print responses until timeout (~1.5s window)
    char js[POC_MAX_JSON];
    int printed = 0;
    while(1){
        size_t nfds=16; int fds[16]={0}; int got = recv_frame_json_with_fds(fd, js, sizeof js, fds, &nfds, 1500);
        if(got<=0) break;
        printf("%s%s\n", js, nfds? "  /* +FDs */": "");
        printed = 1;
    }
    close(fd);
    return printed ? 0 : 1;
}
