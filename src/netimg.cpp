/* 
 * Copyright (c) 2014, 2015 University of Michigan, Ann Arbor.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of Michigan, Ann Arbor. The name of the University 
 * may not be used to endorse or promote products derived from this 
 * software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Author: Sugih Jamin (jamin@eecs.umich.edu)
 *
*/
#include <stdio.h>         // fprintf(), perror(), fflush()
#include <stdlib.h>        // atoi()
#include <assert.h>        // assert()
#include <limits.h>        // LONG_MAX
#include <math.h>          // ceil()
#include <errno.h>
#include <string.h>        // memset(), memcmp(), strlen(), strcpy(), memcpy()
#include <unistd.h>        // getopt(), STDIN_FILENO, gethostname()
#include <signal.h>        // signal()
#include <netdb.h>         // gethostbyname()
#include <netinet/in.h>    // struct in_addr
#include <arpa/inet.h>     // htons()
#include <sys/types.h>     // u_short
#include <sys/socket.h>    // socket API
#include <sys/ioctl.h>     // ioctl(), FIONBIO
#ifdef __APPLE__
#include <GLUT/glut.h>
#else
#include <GL/glut.h>
#endif

#include "netimg.h"
#include "socks.h"
#include "fec.h"          // Lab6

#include <unistd.h>
#include <fcntl.h>

int sd;                   /* socket descriptor */
imsg_t imsg;
long img_size;
unsigned char *image;
unsigned short mss;       // receiver's maximum segment size, in bytes
unsigned char rwnd;       // receiver's window, in packets, of size <= mss
unsigned char fwnd;       // Lab6: receiver's FEC window < rwnd, in packets

unsigned int nextSeqNo_ = 0;
unsigned int missingSeqNo_;
unsigned int numFecSegs_ = 0;
unsigned int currFecSeqNo_ = 0;
unsigned int bytesReceived_ = 0;

/*
 * netimg_args: parses command line args.
 *
 * Returns 0 on success or 1 on failure.  On successful return,
 * "*sname" points to the server's name, and "port" points to the port
 * to connect at server, in network byte order.  Both "*sname", and
 * "port" must be allocated by caller.  The variable "*imgname" points
 * to the name of the image to search for.  The global variables mss
 * and rwnd are initialized.
 *
 * Nothing else is modified.
 */
int
netimg_args(int argc, char *argv[], char **sname, u_short *port, char **imgname)
{
  char c, *p;
  extern char *optarg;
  int arg;

  if (argc < 5) {
    return (1);
  }
  
  rwnd = NETIMG_RCVWIN;
  mss = NETIMG_MSS;

  while ((c = getopt(argc, argv, "s:q:w:m:")) != EOF) {
    switch (c) {
    case 's':
      for (p = optarg+strlen(optarg)-1;  // point to last character of
                                         // addr:port arg
           p != optarg && *p != NETIMG_PORTSEP;
                                         // search for ':' separating
                                         // addr from port
           p--);
      net_assert((p == optarg), "netimg_args: server address malformed");
      *p++ = '\0';
      *port = htons((u_short) atoi(p)); // always stored in network byte order

      net_assert((p-optarg > NETIMG_MAXFNAME),
                 "netimg_args: server's name too long");
      *sname = optarg;
      break;
    case 'q':
      net_assert((strlen(optarg) >= NETIMG_MAXFNAME),
                 "netimg_args: image name too long");
      *imgname = optarg;
      break;
    case 'w':
      arg = atoi(optarg);
      if (arg < 1 || arg > NETIMG_MAXWIN) {
        return(1);
      }
      rwnd = (unsigned char) arg; 
      break;
    case 'm':
      arg = atoi(optarg);
      if (arg < NETIMG_MINSS) {
        return(1);
      }
      mss = (unsigned short) arg;
      break;
    default:
      return(1);
      break;
    }
  }

  return (0);
}

/*
 * netimg_sendqry: send a query for provided imgname to
 * connected server.  Query is of type iqry_t, defined in netimg.h.
 * The query packet must be of version NETIMG_VERS and of type
 * NETIMG_SYNQRY both also defined in netimg.h. In addition to the
 * filename of the image the client is searching for, the query
 * message also carries the receiver's window size (rwnd), maximum
 * segment size (mss), and FEC window size (used in Lab6).
 * All three are global variables.
 *
 * On send error, return 0, else return 1
 */
int
netimg_sendqry(char *imgname)
{
  int bytes;
  iqry_t iqry;

  iqry.iq_vers = NETIMG_VERS;
  iqry.iq_type = NETIMG_SYNQRY;
  iqry.iq_mss = htons(mss);      // global
  iqry.iq_rwnd = rwnd;           // global
  iqry.iq_fwnd = fwnd = NETIMG_FECWIN >= rwnd ? rwnd-1 : NETIMG_FECWIN;  // Lab6
  strcpy(iqry.iq_name, imgname); 
  bytes = send(sd, (char *) &iqry, sizeof(iqry_t), 0);
  if (bytes != sizeof(iqry_t)) {
    return(0);
  }

  return(1);
}
  
/*
 * netimg_recvimsg: receive an imsg_t packet from server and store it
 * in the global variable imsg.  The type imsg_t is defined in
 * netimg.h. Return NETIMG_EVERS if packet is of the wrong version.
 * Return NETIMG_ESIZE if packet received is of the wrong size.
 * Otherwise return the content of the im_type field of the received
 * packet. Upon return, all the integer fields of imsg MUST be in HOST
 * BYTE ORDER. If msg_type is NETIMG_FOUND, compute the size of the
 * incoming image and store the size in the global variable
 * "img_size".
 */
char
netimg_recvimsg()
{
  int bytes;
  double imgdsize;

  /* receive imsg packet and check its version and type */
  bytes = recv(sd, (char *) &imsg, sizeof(imsg_t), 0); // imsg global
  if (bytes != sizeof(imsg_t)) {
    return(NETIMG_ESIZE);
  }
  if (imsg.im_vers != NETIMG_VERS) {
    return(NETIMG_EVERS);
  }

  if (imsg.im_type == NETIMG_FOUND) {
    imsg.im_height = ntohs(imsg.im_height);
    imsg.im_width = ntohs(imsg.im_width);
    imsg.im_format = ntohs(imsg.im_format);

    imgdsize = (double) (imsg.im_height*imsg.im_width*(u_short)imsg.im_depth);
    net_assert((imgdsize > (double) LONG_MAX), 
               "netimg_recvimsg: image too big");
    img_size = (long) imgdsize;                 // global
  }

  return((char) imsg.im_type);
}

/* Callback function for GLUT.
 *
 * netimg_recvimg: called by GLUT when idle On each call, receive a
 * chunk of the image from the network and store it in global variable
 * "image" at offset from the start of the buffer as specified in the
 * header of the packet.
 *
 * Terminate process on receive error.
 */
void
netimg_recvimg(void)
{
  ihdr_t hdr;  // memory to hold packet header
   
  /* 
   * Lab5 Task 2:
   * 
   * The image data packet from the server consists of an ihdr_t
   * header followed by a chunk of data.  We want to put the data
   * directly into the buffer pointed to by the global variable
   * "image" without any additional copying. To determine the correct
   * offset from the start of the buffer to put the data into, we
   * first need to retrieve the sequence number stored in the packet
   * header.  Since we're dealing with UDP packet, however, we can't
   * simply read the header off the network, leaving the rest of the
   * packet to be retrieved by subsequent calls to recv(). Instead, we
   * call recv() with flags == MSG_PEEK.  This allows us to retrieve a
   * copy of the header without removing the packet from the receive
   * buffer.
   *
   * Since our socket has been set to non-blocking mode, if there's no
   * packet ready to be retrieved from the socket, the call to recv()
   * will return immediately with return value -1 and the system
   * global variable "errno" set to EAGAIN or EWOULDBLOCK (defined in
   * errno.h).  In which case, this function should simply return to
   * caller.
   * 
   * Once a copy of the header is made to the local variable "hdr",
   * check that it has the correct version number and that it is of
   * type NETIMG_DATA (use bitwise '&' as NETIMG_FEC is also of type
   * NETIMG_DATA).  Terminate process if any error is encountered.
   * Otherwise, convert the size and sequence number in the header
   * to host byte order.
   */
  /* Lab5: YOUR CODE HERE */
  /* DONE */
  int result = recv(sd, (void *) &hdr, sizeof(hdr), MSG_PEEK);
  if (result == -1) {
    net_assert(errno != EAGAIN && errno != EWOULDBLOCK, "Unknown error during netimg recv");
    return;
  }

  net_assert(hdr.ih_vers != NETIMG_VERS, "Invalid ihdr_t version");
  net_assert(!(NETIMG_DATA & hdr.ih_type), "Invalid ihdr_t type received!");

  uint16_t size = ntohs(hdr.ih_size);
  uint32_t seqn = ntohl(hdr.ih_seqn);

  /* Lab5 Task 2
   *
   * Populate a struct msghdr with a pointer to a struct iovec
   * array.  The iovec array should be of size NETIMG_NUMIOV.  The
   * first entry of the iovec should be initialized to point to the
   * header above, which should be re-used for each chunk of data
   * received.
   */
  /* Lab5: YOUR CODE HERE */
  /* DONE */
  struct iovec iovec_arr[NETIMG_NUMIOV];
  iovec_arr[0].iov_base = (void *) &hdr;
  iovec_arr[0].iov_len = sizeof(hdr);


  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = iovec_arr;
  msg.msg_iovlen = NETIMG_NUMIOV;

  if (hdr.ih_type == NETIMG_DATA) {
    /* 
     * Lab5 Task 2
     *
     * Now that we have the offset/seqno information from the packet
     * header, point the second entry of the iovec to the correct
     * offset from the start of the image buffer pointed to by the
     * global variable "image".  Both the offset/seqno and the size of
     * the data to be received into the image buffer are recorded in
     * the packet header retrieved above. Receive the segment "for
     * real" (as opposed to "peeking" as we did above) by calling
     * recvmsg().  We'll be overwriting the information in the "hdr"
     * local variable, so remember to convert the size and sequence
     * number in the header to host byte order again.
     */
    /* Lab5: YOUR CODE HERE */
    /* DONE */
    iovec_arr[1].iov_base = (void *) (image + seqn);
    iovec_arr[1].iov_len = size;

    result = recvmsg(sd, &msg, 0);
    net_assert(result == -1, "Failed to read DATA from wire");

    /* Lab6 Task 2
     *
     * You should handle the case when the FEC data packet itself may be
     * lost, and when multiple packets within an FEC window are lost, and
     * when the first few packets from the subsequent FEC window following a
     * lost FEC data packet are also lost.  Thus in In addition to relying on
     * fwnd and the count of total packets received within an FEC
     * window, you may want to rely on the sequence numbers in arriving
     * packets to determine when you have received an FEC-window full of data
     * bytes.
     *
     * To that end, in addition to keeping track of lost packet offset
     * below, every time a data packet arrives, first check whether
     * you have received an FEC-window full (or more) of data bytes
     * without receiving any FEC packet.  In which case, you need to
     * reposition your FEC window by computing the start of the
     * current FEC window, reset your count of packets received, and
     * determine the next expected packet.
     *
     * Check whether the arriving data packet is the next data packet
     * you're expecting.  If not, you've lost a packet, mark the
     * location of the first lost packet in an FEC window.  If more
     * than one packet is lost, you don't need to mark subsequent
     * losses, just keep a count of the total number of packets received.
     */
    /* Lab6: YOUR CODE HERE */
  
    // Check if we've dropped an FEC pkt 
    unsigned int datasize = mss - sizeof(ihdr_t) - NETIMG_UDPIP;
    unsigned int feq_size_bytes = datasize * fwnd; // size of an FEC window in bytes
    unsigned int next_fec_seq_no = feq_size_bytes + currFecSeqNo_;

    // Check if we've dropped an FEC pkt
    if (seqn >= next_fec_seq_no) {
      fprintf(stderr, "- we've dropped an FEC pkt!\n");
      // We've dropped an FEC pkt, so reset the FEC window to the new packet sequence
      numFecSegs_ = 0;
      currFecSeqNo_ = seqn - (seqn % feq_size_bytes);
      nextSeqNo_ = currFecSeqNo_;
    }

    bool accumulate_pkt = true;

    // Check if we've dropped a data packet
    if (seqn > nextSeqNo_) {
      fprintf(stderr, "- Packets dropped...\n");
      for (unsigned int i = nextSeqNo_; i < seqn; i += datasize) {
        unsigned int dropped_pkt_size = (i + datasize > img_size)
          ? img_size - i 
          : datasize;

        fprintf(stderr, "\t- packet dropped: offset 0x%x, %d bytes\n",
            i, dropped_pkt_size);  
      }

      // We've dropped a data packet, so register the packet as missing
      missingSeqNo_ = nextSeqNo_;

    
    } else if (seqn < nextSeqNo_) {
      fprintf(stderr, "netimg_recvimg: received OUT-OF-ORDER DATA packet: offset 0x%x, %d bytes\n",
              seqn, size);
      
      accumulate_pkt = false;
    }

    if (accumulate_pkt) {
      ++numFecSegs_;
      nextSeqNo_ = seqn + size;
      
      fprintf(stderr, "netimg_recvimg: received DATA offset 0x%x, %d bytes ",
              seqn, size);

      bytesReceived_ += size;
      fprintf(stderr, "-- nextSeqNo_: 0x%x, missingSeqNo_: 0x%x, currFecSeqNo_: 0x%x, numFecSegs_: %u\n",
          nextSeqNo_, missingSeqNo_, currFecSeqNo_, numFecSegs_);
    }


  } else { // FEC pkt
    /* Lab6 Task 2
     *
     * Re-use the same struct msghdr above to receive an FEC packet.
     * Point the second entry of the iovec to your FEC data buffer and
     * update the size accordingly.  Receive the segment by calling
     * recvmsg().
     *
     * Convert the size and sequence number in the header to host byte
     * order.
     *
     * This is an adaptation of your Lab5 code.
     */
    /* Lab6: YOUR CODE HERE */
    fprintf(stderr, "netimg_recvimg: received FEC offset 0x%x, %d bytes\n",
            seqn, size);
    
    unsigned int datasize = mss - sizeof(ihdr_t) - NETIMG_UDPIP;
    net_assert(datasize != size, "size does not equal datasize");

    unsigned char * fec_buff = new unsigned char[datasize];
    memset(fec_buff, 0, datasize);
    iovec_arr[1].iov_base = (void *) fec_buff;
    iovec_arr[1].iov_len = size;
    int fec_result = recvmsg(sd, &msg, 0);
    net_assert(fec_result == -1, "Failed to read FEC pkt from wire");

    /* Lab6 Task 2
     *
     * Check if you've lost only one packet within the FEC window, if
     * so, reconstruct the lost packet.  Remember that we're using the
     * image data buffer itself as our FEC buffer and that you've
     * noted above the sequence number that marks the start of the
     * current FEC window.  To reconstruct the lost packet, use
     * fec.cpp:fec_accum() to XOR the received FEC data against the
     * image data buffered starting from the start of the current FEC
     * window, one <tt>datasize</tt> at a time, skipping over the lost
     * segment, until you've reached the end of the FEC window.  If
     * fec_accum() has been coded correctly, it should be able to
     * correcly handle the case when the last segment of the
     * FEC-window is smaller than datasize *(but you must still do the
     * detection for short last segment here and provide fec_accum()
     * with the appropriate segsize)*.
     *
     * Once you've reconstructed the lost segment, copy it from the
     * FEC data buffer to correct offset on the image buffer.  You
     * must be careful that if the lost segment is the last segment of
     * the image data, it may be of size smaller than datasize, in
     * which case, you should copy only the correct amount of bytes
     * from the FEC data buffer to the image data buffer.  If no
     * packet was lost in the current FEC window, or if more than one
     * packets were lost, there's nothing further to do with the
     * current FEC window, just move on to the next one.
     *
     * Before you move on to the next FEC window, you may want to
     * reset your FEC-window related variables to prepare for the
     * processing of the next window.
     */
    /* Lab6: YOUR CODE HERE */

    // Check for skipped data segments
    unsigned int next_fec_seqno = currFecSeqNo_ + fwnd * datasize;
    if (seqn == next_fec_seqno) {
      for (unsigned int i = nextSeqNo_; i < seqn; i += datasize) {
        missingSeqNo_ = i;
        unsigned int dropped_pkt_size = (i + datasize > img_size)
          ? img_size - i 
          : datasize;

        fprintf(stderr, "\t- packet dropped: offset 0x%x, %d bytes\n",
            i, dropped_pkt_size);  
      }
      nextSeqNo_ = next_fec_seqno;
    }

    // Check if we've received the correct FEC pkt and that we should
    // reconstruct a single missing pkt
    if (seqn == nextSeqNo_ && numFecSegs_ == fwnd - 1) {
      unsigned int last_segment_seqno = img_size - (img_size % datasize);
      // Use simple XOR magic to recover the missing packet
      for (unsigned int i = currFecSeqNo_; i < seqn; i += datasize) {
        
        // DOn't XOR the missing segment
        if (i != missingSeqNo_) {
          unsigned int segsize = (i == last_segment_seqno)
              ? img_size % datasize
              : datasize;
          fprintf(stderr, "- XOR details: seqn: 0x%x, datasize: %u, segsize: %u\n",
              i, datasize, segsize);
          fec_accum(fec_buff, image + i, datasize, segsize);
        }
      }

      // Copy the reconstructed packe to its proper place in the image buffer
      unsigned int missing_segment_size = (missingSeqNo_ == last_segment_seqno)
          ? img_size - last_segment_seqno
          : datasize;
      
      fprintf(stderr, "- Using FEC simple XOR to recover segment with offset 0x%x and size %d\n",
          missingSeqNo_, missing_segment_size);

      memcpy(image + missingSeqNo_, fec_buff, missing_segment_size);
    } 

    // Reset FEC window
    numFecSegs_ = 0;
    nextSeqNo_ = seqn;
    currFecSeqNo_ = seqn;
    
    fprintf(stderr, "- Reseting FEC window to numFecSegs_: %u, nextSeqNo_: 0x%x, currFecSeqNo_: 0x%x\n",
        numFecSegs_, nextSeqNo_, currFecSeqNo_);

    delete[] fec_buff;
  }
  
  /* give the updated image to OpenGL for texturing */
  glTexImage2D(GL_TEXTURE_2D, 0, (GLint) imsg.im_format,
               (GLsizei) imsg.im_width, (GLsizei) imsg.im_height, 0,
               (GLenum) imsg.im_format, GL_UNSIGNED_BYTE, image);
  /* redisplay */
  glutPostRedisplay();

  return;
}

int
main(int argc, char *argv[])
{
  int err;
  char *sname, *imgname;
  u_short port;

  // parse args, see the comments for netimg_args()
  if (netimg_args(argc, argv, &sname, &port, &imgname)) {
    fprintf(stderr, "Usage: %s -s <server>%c<port> -q <image>.tga [ -w <rwnd [1, 255]> -m <mss (>40)> ]\n", argv[0], NETIMG_PORTSEP); 
    exit(1);
  }

  srandom(NETIMG_SEED);

  socks_init();

  sd = socks_clntinit(sname, port, rwnd*mss);  // Lab5 Task 2

  if (netimg_sendqry(imgname)) {
    err = netimg_recvimsg();

    if (err == NETIMG_FOUND) { // if image received ok
      netimg_glutinit(&argc, argv, netimg_recvimg);
      netimg_imginit(imsg.im_format);
      
      /* Lab5 Task 2: set socket non blocking */
      /* Lab5: YOUR CODE HERE */
      /* DONE */
      net_assert(fcntl(sd, F_SETFL, O_NONBLOCK) == -1, "Failed to set socket to non-blocking");

      glutMainLoop(); /* start the GLUT main loop */
    } else if (err == NETIMG_NFOUND) {
      fprintf(stderr, "%s: %s image not found.\n", argv[0], imgname);
    } else if (err == NETIMG_EVERS) {
      fprintf(stderr, "%s: wrong version number.\n", argv[0]);
    } else if (err == NETIMG_EBUSY) {
      fprintf(stderr, "%s: image server busy.\n", argv[0]);
    } else if (err == NETIMG_ESIZE) {
      fprintf(stderr, "%s: wrong size.\n", argv[0]);
    } else {
      fprintf(stderr, "%s: image receive error %d.\n", argv[0], err);
    }
  }

  socks_close(sd); // optional, but since we use connect(), might as well.
  return(0);
}
