//
//  device-uart.h
//

#ifndef rv_device_uart_h
#define rv_device_uart_h

namespace riscv {

	/* Console Thread */

	template <typename P>
	struct console_thread
	{
		P &proc;
		struct termios old_tio, new_tio;
		int pipefds[2];
		struct pollfd pollfds[2];
		queue_atomic<char> queue;
		volatile bool running;
		volatile bool suspended;
		std::thread thread;

		console_thread(P &proc) :
			proc(proc),
			pipefds{0},
			pollfds(),
			queue(1024),
			running(true),
			suspended(false),
			thread(&console_thread::mainloop, this)
		{}

		~console_thread()
		{
			shutdown();
		}

		void mainloop()
		{
			block_signals();
			open_pipe();
			configure_console();
			while (running) {
				pollfds[0].fd = pipefds[0];
				pollfds[0].events = POLLIN;
				pollfds[0].revents = 0;
				pollfds[1].fd = STDIN_FILENO;
				pollfds[1].events = POLLIN;
				pollfds[1].revents = 0;
				if (poll(pollfds, 2, -1) < 0 && errno != EINTR) {
					panic("console poll failed: %s", strerror(errno));
				}
				if (!running) break;
				if (pollfds[0].revents & POLLIN) {
					unsigned char c;
					if (read(pipefds[0], &c, 1) < 0) {
						debug("console: socket: read: %s", strerror(errno));
					} else if (write(STDIN_FILENO, &c, 1) < 0) {
						debug("console: socket: write: %s", strerror(errno));
					}
				}
				if (suspended) continue;
				if (pollfds[1].revents & POLLIN) {
					unsigned char c;
					if (read(STDIN_FILENO, &c, 1) < 0) {
						debug("console: stdin: read: %s", strerror(errno));
					} else {
						queue.push_back(c);
					}
				}
			}
			restore_console();
			close_pipe();
		}

		void block_signals()
		{
			// block signals
			sigset_t set;
			sigemptyset(&set);
			sigaddset(&set, SIGSEGV);
			sigaddset(&set, SIGTERM);
			sigaddset(&set, SIGQUIT);
			sigaddset(&set, SIGINT);
			sigaddset(&set, SIGHUP);
			sigaddset(&set, SIGUSR1);
			if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0)
			{
				panic("console_thread: can't set thread signal mask: %s",
					strerror(errno));
			}
		}

		void open_pipe()
		{
			/* create socketpair */
			if (pipe(pipefds) < 0) {
				panic("pipe failed: %s", strerror(errno));
			}
			if (fcntl(pipefds[0], F_SETFD, FD_CLOEXEC) < 0) {
				panic("console fcntl(F_SETFD, FD_CLOEXEC) failed: %s", strerror(errno));
			}
			if (fcntl(pipefds[0], F_SETFL, O_NONBLOCK) < 0) {
				panic("console fcntl(F_SETFL, O_NONBLOCK) failed: %s", strerror(errno));
			}
			if (fcntl(pipefds[1], F_SETFD, FD_CLOEXEC) < 0) {
				panic("console fcntl(F_SETFD, FD_CLOEXEC) failed: %s", strerror(errno));
			}
			if (fcntl(pipefds[1], F_SETFL, O_NONBLOCK) < 0) {
				panic("console fcntl(F_SETFL, O_NONBLOCK) failed: %s", strerror(errno));
			}
		}

		void close_pipe() {
			close(pipefds[0]);
			close(pipefds[1]);
		}

		/* setup console */
		void configure_console()
		{
			tcgetattr(STDIN_FILENO, &old_tio);
			new_tio = old_tio;
			new_tio.c_lflag &=(~ICANON & ~ECHO);
			tcsetattr(STDIN_FILENO,TCSANOW,&new_tio);
		}

		/* teardown console */
		void restore_console()
		{
			/* restore settings */
			tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
		}

		/* suspend console input */
		void suspend()
		{
			restore_console();
			suspended = true;
		}

		/* resume console input */
		void resume()
		{
			configure_console();
			suspended = false;
		}

		/* shutdown console thread */
		void shutdown()
		{
			/* set running flag to false and write a null byte to the FIFO */
			running = false;
			unsigned char c = 0;
			/* wake up the console thread so it checks running and exits */
			if (write(pipefds[1], &c, 1) < 0) {
				debug("console: socket: write: %s", strerror(errno));
			}
			/* wait for the console thread to finish */
			thread.join();
		}

		/* check if data is available */
		bool has_char()
		{
			return queue.size() > 0;
		}

		/* read one character */
		u8 read_char()
		{
			return queue.size() > 0 ? queue.pop_front() : 0;
		}

		/* write one character */
		void write_char(u8 c)
		{
			if (write(pipefds[1], &c, 1) < 0) {
				debug("console: socket: write: %s", strerror(errno));
			}
		}
	};

	/* UART MMIO device */

	template <typename P>
	struct uart_mmio_device : memory_segment<typename P::ux>
	{
		typedef typename P::ux UX;
		typedef std::shared_ptr<console_thread<P>> console_thread_ptr;
		typedef std::shared_ptr<plic_mmio_device<P>> plic_mmio_device_ptr;

		P &proc;
		plic_mmio_device_ptr plic;
		UX irq;
		console_thread_ptr console;

		/*
		 * UART Registers
		 *
		 * Register MMIO offsets in brackets (Based on 16550)
		 *
		 * Reference: https://www.lammertbies.nl/comm/info/serial-uart.html
		 */

		struct {
			u8 rbr;              /* (R  [0]) Recieve Buffer Register        */
			u8 thr;              /* (W  [0]) Transmit Holding Register      */
			u8 ier;              /* (RW [1]) Interrupt Enable Register      */
			u8 iir;              /* (R  [2]) Interrupt Identity Register    */
			u8 fcr;              /* (W  [2]) FIFO Control Register          */
			u8 lcr;              /* (RW [3]) Line Control Register          */
			u8 mcr;              /* (RW [4]) MODEM Control Register         */
			u8 lsr;              /* (RW [5]) Line Status Register           */
			u8 msr;              /* (RW [6]) MODEM Status Register          */
			u8 scr;              /* (RW [7]) Scratch Register               */
			u8 dll;              /* (RW [0]) Divisor Latch LSB (LCR.DLAB=1) */
			u8 dlm;              /* (RW [1]) Divisor Latch MSB (LCR.DLAB=1) */
		} com;

		enum {
			REG_RBR      = 0,
			REG_THR      = 0,
			REG_DLL      = 0,
			REG_IER      = 1,
			REG_DLM      = 1,
			REG_IIR      = 2,
			REG_FCR      = 2,
			REG_LCR      = 3,
			REG_MCR      = 4,
			REG_LSR      = 5,
			REG_MSR      = 6,
			REG_SCR      = 7,

			IER_ERBDA    = 0x01,  /* Enable Recieved Buffer Data Available Interrupt */
			IER_ETHRE    = 0x02,  /* Enable Transmitter Holding Register Empty Interrupt */
			IER_ERLS     = 0x04,  /* Enable Reciever Line Status Interrupt */
			IER_EMSC     = 0x08,  /* Enable Modem Status Interrupt */
			IER_MASK     = 0x0f,  /* Interrupt Enable Mask */

			IIR_NOPEND   = 0x01,  /* No Interrupt Pending */
			IIR_RD_MSR   = 0x00,  /* Modem Status Change (Reset by MSR read) */
			IIR_TX_RDY   = 0x02,  /* Transmit Ready      (Reset by IIR read or THR write) */
			IIR_RX_RDY   = 0x04,  /* Receive Ready       (Reset by RBR read) */
			IIR_RD_LSR   = 0x06,  /* Read Line Status    (Reset by LSR read) */
			IIR_TIMEOUT  = 0x0c,  /* Read Timeout        (Reset by LSR read) */
			IIR_MASK     = 0x0f,  /* Interrupt Identification Mask */
			IIR_FIFO     = 0xc0,  /* FIFO Enabled */

			FCR_ENABLE   = 0x01,  /* FIFO Enable */
			FCR_RX_CLR   = 0x02,  /* FIFO Receive Clear */
			FCR_TX_CLR   = 0x04,  /* FIFO Transmit Clear */
			FCR_DMA      = 0x08,  /* FIFO DMA */
			FCR_RX_MASK  = 0xc0,  /* FIFO Trigger Mask (1,4,8,14) */

			LCR_5BIT     = 0x00,  /* 5-bit */
			LCR_6BIT     = 0x01,  /* 6-bit */
			LCR_7BIT     = 0x02,  /* 7-bit */
			LCR_8BIT     = 0x03,  /* 8-bit */
			LCR_BMASK    = 0x07,  /* Bit mask */
			LCR_STOPB    = 0x04,  /* Stop bit */
			LCR_PNONE    = 0x00,  /* Parity None */
			LCR_PODD     = 0x08,  /* Parity Odd */
			LCR_PEVEN    = 0x18,  /* Parity Even */
			LCR_PHIGH    = 0x28,  /* Parity High */
			LCR_PLOW     = 0x38,  /* Parity Low */
			LCR_PMASK    = 0x38,  /* Parity Mask */
			LCR_BREAK    = 0x40,  /* Break Signal Enabled */
			LCR_DLAB     = 0x80,  /* Divisor Latch Bit */

			MCR_DTR      = 0x01,  /* Data Terminal Ready */
			MCR_RTS      = 0x02,  /* Request To Send */
			MCR_OUT1     = 0x04,  /* Output 1 */
			MCR_OUT2     = 0x08,  /* Output 2 */
			MCR_LOOP     = 0x10,  /* Loopback Mode */

			LSR_DA       = 0x01,  /* Data Available */
			LSR_OE       = 0x02,  /* Overrun Error */
			LSR_PE       = 0x04,  /* Parity Error */
			LSR_FE       = 0x08,  /* Framing Error */
			LSR_BS       = 0x10,  /* Break Signal */
			LSR_RE       = 0x20,  /* THR is empty */
			LSR_RI       = 0x40,  /* THR is empty and line is idle */
			LSR_EF       = 0x80,  /* Erroneous data in FIFO */

			MSR_DCTS     = 0x01,  /* Delta Clear To Send */
			MSR_DDSR     = 0x02,  /* Delta Data Set Ready */
			MSR_TERI     = 0x04,  /* Trailing Edge Ring Indicator */
			MSR_DDCD     = 0x08,  /* Delta Carrier Detect */
			MSR_CTS      = 0x10,  /* Clear to Send */
			MSR_DSR      = 0x20,  /* Data Set Ready */
			MSR_RI       = 0x40,  /* Ring Indicator */
			MSR_DCD      = 0x80,  /* Data Carrier Detect */

			FIFOSZ       = 16,    /* FIFO size costant */
		};

		/* UART constructor */

		uart_mmio_device(P &proc, UX mpa, plic_mmio_device_ptr plic, UX irq) :
			memory_segment<UX>("UART", mpa, /*uva*/0, /*size*/8,
				pma_type_io | pma_prot_read | pma_prot_write),
			proc(proc),
			plic(plic),
			irq(irq),
			com{0}
		{
			console = std::make_shared<console_thread<P>>(proc);
		}

		void service()
		{
			if ((com.ier & IER_ERBDA) && console->has_char()) {
				plic->signal_irq(irq);
			}
		}

		void print_registers()
		{
			debug("uart_mmio:rbr              %d", com.rbr);
			debug("uart_mmio:thr              %d", com.thr);
			debug("uart_mmio:ier              %d", com.ier);
			debug("uart_mmio:iir              %d", com.iir);
			debug("uart_mmio:fcr              %d", com.fcr);
			debug("uart_mmio:lcr              %d", com.lcr);
			debug("uart_mmio:mcr              %d", com.mcr);
			debug("uart_mmio:lsr              %d", com.lsr);
			debug("uart_mmio:msr              %d", com.msr);
			debug("uart_mmio:scr              %d", com.scr);
			debug("uart_mmio:dll              %d", com.dll);
			debug("uart_mmio:dlm              %d", com.dlm);
		}

		/* UART MMIO interface */

		void load_8 (UX va, u8  &val)
		{
			if (com.lcr & LCR_DLAB) {
				switch (va) {
				case REG_DLL: /* Divisor Latch LSB */
					val = com.dll;
					break;
				case REG_DLM: /* Divisor Latch MSB */
					val = com.dlm;
					break;
				default:
					break;
				}
			} else {
				switch (va) {
				case REG_RBR: /* Recieve Buffer Register */
					if (console->has_char()) com.rbr = console->read_char();
					val = com.rbr;
					break;
				case REG_IER: /* Interrupt Enable Register */
					val = com.ier;
				case REG_IIR: /* Interrupt Identity Register */
					val = console->has_char() ? IIR_RD_LSR : IIR_TX_RDY;
					break;
				case REG_LCR: /* Line Control Register */
					val = com.lcr;
					break;
				case REG_MCR: /* MODEM Control Register */
					val = com.mcr;
					break;
				case REG_LSR: /* Line Status Register */
					val = LSR_RE | (console->has_char() ? LSR_DA : 0);
					break;
				case REG_MSR: /* MODEM Status Register */
					val = MSR_DCD | MSR_DSR;
					break;
				case REG_SCR: /* Scratch Register */
					val = com.scr;
					break;
				default:
					val = 0;
					break;
				}
			}
			if (proc.log & proc_log_mmio) {
				printf("uart_mmio:0x%04llx -> 0x%hhx\n", addr_t(va), val);
			}
		}

		void store_8 (UX va, u8  val)
		{
			if (proc.log & proc_log_mmio) {
				printf("uart_mmio:0x%04llx <- 0x%hhx\n", addr_t(va), val);
			}
			if (com.lcr & LCR_DLAB) {
				switch (va) {
				case REG_DLL: /* Divisor Latch LSB */
					com.dll = val;
					break;
				case REG_DLM: /* Divisor Latch MSB */
					com.dlm = val;
					break;
				default:
					break;
				}
			} else {
				switch (va) {
				case REG_THR: /* Transmist Holding Register */
					com.thr = val;
					console->write_char(val);
					break;
				case REG_IER: /* Interrupt Enable Register */
					com.ier = val & IER_MASK;
					break;
				case REG_FCR: /* FIFO Control Register */
					/* ignore writes */
					break;
				case REG_LCR: /* Line Control Register */
					com.lcr = val;
					break;
				case REG_MCR: /* MODEM Control Register */
					com.mcr = val;
					break;
				case REG_LSR: /* Line Status Register */
					/* ignore writes */
					break;
				case REG_MSR: /* MODEM Status Register */
					/* ignore writes */
					break;
				case REG_SCR: /* Scratch Register */
					com.scr = val;
					break;
				default:
					break;
				}
			}
		}

		/* UART implementation */

	};

}

#endif