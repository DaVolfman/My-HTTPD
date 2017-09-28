default: myhttpd.c
	gcc -pthread -o httpd.my myhttpd.c
clean:
	$(RM) httpd.my

