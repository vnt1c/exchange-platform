# Bourse Exchange Platform

A multi-threaded trading exchange server that matches buy and sell orders in real-time. The Bourse exchange allows traders to connect via TCP, manage their accounts, and execute trades.

## Features

- **Multi-threaded server**: Handles multiple client connections concurrently
- **Order matching**: Automatically matches buy and sell orders based on price ranges
- **Account management**: Deposit, withdraw, and manage inventory
- **Real-time notifications**: Traders receive updates about trades, order postings, and cancellations
- **Graceful shutdown**: Supports clean termination via SIGHUP signal

## Prerequisites

- **GCC compiler** with C11 support
- **pthread library** (usually included with GCC)
- **Criterion testing framework** (optional, for running tests)

### Installing Criterion (for tests)

On macOS:
```bash
brew install criterion
```

On Ubuntu/Debian:
```bash
sudo apt-get install libcriterion-dev
```

## Building the Project

### Standard Build

To build the project in release mode:

```bash
make
```

This will:
- Create `build/` and `bin/` directories
- Compile all source files
- Create the main executable `bin/bourse`
- Create the test executable `bin/bourse_tests`

### Debug Build

To build with debug symbols and debug output:

```bash
make debug
```

This enables:
- Debug symbols (`-g`)
- Debug macros (`-DDEBUG`)
- Color output (`-DCOLOR`)
- Print statements for errors, warnings, info, and success messages

### Clean Build

To remove all build artifacts:

```bash
make clean
```

## Running the Server

Start the Bourse exchange server with:

```bash
./bin/bourse -p <port>
```

Where `<port>` is the port number (0-65535) on which the server should listen for client connections.

**Example:**
```bash
./bin/bourse -p 9999
```

The server will:
- Listen for incoming client connections on the specified port
- Create a new thread for each connected client
- Process trading requests and match orders automatically
- Broadcast trade notifications to all connected traders

### Shutting Down the Server

To gracefully shut down the server, send a SIGHUP signal:

```bash
kill -HUP <pid>
```

Or if running in the foreground, press `Ctrl+C` (though SIGHUP is the recommended method).

The server will:
- Close all client connections
- Wait for all service threads to terminate
- Clean up resources
- Exit cleanly

## Connecting Clients

The project includes a client utility in `util/client` that can be used to connect to the server. The client utility is a binary executable.

**Example:**
```bash
./util/client <hostname> <port>
```

## Protocol Overview

The Bourse protocol uses TCP for full-duplex communication. Clients and servers exchange packets with fixed headers and optional payloads.

### Client-to-Server Packets

- **LOGIN**: Log a trader into the exchange
- **STATUS**: Request balance, inventory, and market information
- **DEPOSIT**: Deposit funds into trader's account
- **WITHDRAW**: Withdraw funds from trader's account
- **ESCROW**: Place inventory in escrow
- **RELEASE**: Release inventory from escrow
- **BUY**: Post a buy order
- **SELL**: Post a sell order
- **CANCEL**: Cancel a pending order

### Server-to-Client Packets

**Synchronous responses:**
- **ACK**: Successful request acknowledgment
- **NACK**: Failed request notification

**Asynchronous notifications:**
- **BOUGHT**: Client's buy order was fulfilled
- **SOLD**: Client's sell order was fulfilled
- **POSTED**: A new order was posted (broadcast to all)
- **CANCELED**: An order was canceled (broadcast to all)
- **TRADED**: A trade occurred (broadcast to all)

## Running Tests

To run the test suite:

```bash
./bin/bourse_tests
```

Make sure you have Criterion installed before building tests.

## Project Structure

```
exchange-platform/
├── include/          # Header files
│   ├── account.h     # Account management
│   ├── client_registry.h  # Client connection tracking
│   ├── exchange.h    # Exchange order matching
│   ├── protocol.h    # Protocol definitions
│   ├── server.h      # Server implementation
│   └── trader.h      # Trader management
├── src/              # Source files
│   ├── main.c        # Server entry point
│   ├── account.c
│   ├── client_registry.c
│   ├── exchange.c
│   ├── protocol.c
│   ├── server.c
│   └── trader.c
├── tests/            # Test files
│   └── bourse_tests.c
├── util/             # Utilities
│   └── client        # Client utility (binary)
├── Makefile          # Build configuration
└── README.md         # This file
```

## Order Matching Algorithm

The exchange uses a matchmaker thread that:

1. Sleeps until a change in pending orders occurs
2. Finds matching buy and sell orders (where min sell price ≤ max buy price)
3. Executes trades at the price closest to the last trade price within the overlap range
4. Trades the minimum quantity between the two orders
5. Updates accounts and inventory accordingly
6. Broadcasts notifications to all traders
7. Removes orders with zero remaining quantity

## Thread Safety

The exchange is designed to be thread-safe:
- Multiple clients can connect simultaneously
- Each client is handled by a separate thread
- The matchmaker thread runs independently
- Proper synchronization is used for shared data structures

## Troubleshooting

### Port Already in Use

If you get an error about the port being in use:
```bash
# Find the process using the port
lsof -i :<port>

# Kill the process if needed
kill -9 <pid>
```

### Build Errors

- Ensure you have GCC installed: `gcc --version`
- Check that pthread is available (usually included)
- For tests, ensure Criterion is installed

### Connection Issues

- Verify the server is running: `ps aux | grep bourse`
- Check firewall settings
- Ensure the port number is valid (0-65535)

## License

This project appears to be an educational assignment. Please refer to your course materials for licensing information.

