/* FFmpeg PSP 0.5 was built for old newlib, which exported this pointer.
   Modern PSPSDK exports the same classification table as _ctype_. */
extern const char _ctype_[];
char *__ctype_ptr__ = (char *)_ctype_;
