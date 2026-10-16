idf_component_register(
    SRCS main.c battery.c power.c power_decide.c store.c wifi.c modem.c net.c portal.c cam.c flybot.c ota.c proto.c shadow.c sched.c
         util.c tz.c nmea.c gps.c live.c console.c
    INCLUDE_DIRS "."
    EMBED_TXTFILES certs/AmazonRootCA1.pem portal.html
)
