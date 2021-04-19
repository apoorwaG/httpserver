To run the program, one must enter the name of the executable with a required port number
and optional arguments -N that specifies the number of desired threads and -l that specifies
the file where each request is logged.

Examples: ./httpserver 8080 -N 4 -l logfile.txt
	 ./httpserver 8080
         ./httpserver 8080 -l logfile.txt

The server would then accept requests from clients, in the same manner as Assignment 1.
An additional option/request healtcheck is available as a form of GET request that lets
the client know of the server's overall performance.

Example Client requests:
GET request: curl http://localhost:8080/file_name
PUT request: curl -T file_name http://localhost:8080/file_name
HEAD request: curl -I http://localhost:8080/file_name

The multithreaded server produces unsatisfactory results when PUT and GET requests on the same
file are performed by clients. Sometimes, it returns satisfactory results, but most of the time, GET's results aren't displayed.

