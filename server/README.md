# Mouffette Server

Node.js WebSocket server for the Mouffette media sharing application.

## Features

- WebSocket-based client communication
- Client registration and discovery
- Real-time media sharing coordination
- Multi-screen support
- Cross-platform client support

## Installation

```bash
cd server
npm install
```

## Running

### Development mode (with auto-restart)
```bash
npm run dev
```

### Production mode
```bash
npm start
```

The server will start on port 8080 by default.

## API

### WebSocket Messages

#### Client to Server:

1. **Register Client**
```json
{
  "type": "register",
  "machineName": "Alice's MacBook",
  "platform": "macOS",
  "screens": [
    {"id": 0, "width": 1920, "height": 1080, "primary": true},
    {"id": 1, "width": 1440, "height": 900, "primary": false}
  ]
}
```

2. **Request Client List**
```json
{
  "type": "request_client_list"
}
```

3. **Share Media**
```json
{
  "type": "media_share",
  "targetClientId": "target-uuid",
  "mediaData": [...],
  "screens": {...}
}
```

#### Server to Client:

1. **Welcome Message**
```json
{
  "type": "welcome",
  "clientId": "client-uuid",
  "message": "Connected to Mouffette Server"
}
```

2. **Client List**
```json
{
  "type": "client_list",
  "clients": [
    {
      "id": "client-uuid",
      "machineName": "Bob's PC",
      "platform": "Windows",
      "screens": [...],
      "status": "connected"
    }
  ]
}
```

## Development

The server uses:
- `ws` for WebSocket functionality
- `uuid` for unique client identification
- `nodemon` for development auto-restart
