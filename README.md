# Concurrent Server

Implemented a lightwight concurrent server based on thread pool and message queue by using C language. The concurrent server can bind and listen to multiple ports, respond to download requests from multiple clients and return file streams

In the program, I created a FIFO queue structure, and realized queue initialization, entering, exiting and destroying interfaces, realizing the decoupling of the receiving system and the sending system and asynchronous processing of the interface
