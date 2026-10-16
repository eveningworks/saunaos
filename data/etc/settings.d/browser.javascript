# /etc/settings.d/browser.javascript -- whether web pages may run scripts
# (userland/webcontent/wc_js.c reads it for each page it creates; a site's
# own Allow or Block, kept by the Web Browser, wins over it).
Type=enum
File=/etc/browser.conf
Label=JavaScript
Category=Network
Group=Web Browser
Default=on
Choices=off,on
Choice.off=Off
Choice.on=On
Description=Lets web pages run scripts. Applies to pages loaded after the change
Order=20
