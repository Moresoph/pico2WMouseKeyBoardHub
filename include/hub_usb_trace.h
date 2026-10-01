#ifndef HUB_USB_TRACE_H
#define HUB_USB_TRACE_H

int hub_usb_trace_printf(const char *format, ...);
void hub_usb_trace_dump(void);
void hub_usb_trace_poll(void);
void hub_usb_trace_status(void);

#endif
