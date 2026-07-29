#ifndef UTUBBU_DIAGNOSTIC_H
#define UTUBBU_DIAGNOSTIC_H

/* The public build deliberately performs no diagnostic file logging. */
#define utubbu_log_reset() ((void)0)
#define utubbu_log(stage, value) ((void)0)
#define utubbu_log_text(stage, value, length) ((void)0)

#endif
