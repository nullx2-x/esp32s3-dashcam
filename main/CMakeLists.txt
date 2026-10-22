idf_component_register(
    SRCS
        "main.c"
        "avi.c"
        "camera.c"
        "dns_server.c"
        "logbuf.c"
        "net.c"
        "recorder.c"
        "settings.c"
        "storage.c"
        "usb_uvc.c"
        "web.c"
    INCLUDE_DIRS "."
    EMBED_FILES "www/index.html"
)
