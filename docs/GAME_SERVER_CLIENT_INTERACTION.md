# Game Server Client Interaction

## Boundary

External game clients should connect to a gateway service, not to `tunnel`, `global`, or `zone` directly.

`tunnel`, `global`, and `zone` use internal RPC. That RPC is optimized for trusted service-to-service calls and service-id routing. Client traffic needs a different boundary because it requires authentication, session state, rate limits, protocol compatibility, reconnect handling, and anti-cheat checks.

## Topology

```text
client
  -> web login/register
  -> gateway after auth
  -> tunnel
  -> world / zone / other internal services

web
  -> Redis service process through redis_cli
  -> world directly for login options
```

The gateway owns the client connection. Internal services receive only validated internal RPC messages.

The web process does not embed or start Redis. Redis is an external service process, normally local in development, and `web` connects to it through `libs/redis_cli` using `redis_host`, `redis_port`, `redis_db`, optional username/password, and timeout config.

## Gateway Responsibilities

- Accept client connections with the public protocol, such as TCP, WebSocket, KCP, QUIC, or a platform-specific transport.
- Authenticate the client and bind the connection to an account, role, or session id.
- Decode client packets into internal commands.
- Validate packet sequence, size, rate, and authorization.
- Resolve the target internal service by player state, scene id, shard id, or explicit routing metadata.
- Forward validated requests through tunnel using service id routing.
- Translate internal responses back to client packets.
- Push server events back to the owning client connection.

## Internal Routing

The gateway should register itself to tunnel like every other service. Its `GameServiceId` uses `GameServiceType::gateway`.

For request routing, gateway builds a `TunnelEnvelope` and chooses one of the existing forward modes:

- `specific`: route to one concrete service instance, such as a player's current zone.
- `random_one`: route to any service of a type, such as stateless match/query service.
- `all_of_type`: broadcast to every service of a type, usually admin or system events.

For responses or pushes back to clients, the target is the gateway instance that owns the session.

## Client Session Mapping

Gateway should maintain mappings similar to:

```text
connection_id -> session
player_id -> connection_id
player_id -> current_zone_service_id
request_id -> client callback / continuation
```

Internal services should not hold socket handles or client connections. They should return responses or push events to gateway by service id.

## Login Flow

Current local test flow:

```text
client -> web.register / web.login
web -> Redis via redis_cli
web -> world.login.options directly
client -> gateway.login with selected role
gateway -> world.zone.select
gateway -> tunnel.forward -> zone.player.enter
client -> gateway.game.forward after login
gateway -> tunnel.forward -> zone
```

`web` returns `WebAuthResponse` only after successful auth. That response includes the `PlayerUid` and `LoginOptionsResponse`, including available gateways and roles. A role is bound to one world by `PlayerRoleInfo::world_service_id`.

## Protocol Choice

The public client protocol does not need to be identical to internal RPC.

Recommended split:

- Client wire protocol: stable, versioned, compact, and compatible with game clients.
- Internal RPC protocol: current `YuanRpc` frame/message model.
- Gateway adapter: translates between the two.

If a client can safely use the same binary message schema as internal services, it can share codecs. It still should not bypass gateway.

## Security Model

Do not expose internal RPC endpoints to the public network.

Gateway should be the public security boundary:

- TLS or transport encryption if needed.
- Login token verification.
- Session resume checks.
- Packet size limits.
- Rate limits.
- Replay/sequence checks.
- Permission checks before forwarding.

Internal RPC may later add service-to-service authentication, but that does not replace gateway validation.

## Current Implementation Status

Implemented now:

- Internal RPC protocol in `libs/rpc`.
- Core-backed RPC transport facade in `libs/game/server/common/rpc_network.*`.
- `RpcNetworkServer` and `RpcNetworkClient` names for game server usage.
- Service id routing in tunnel.
- `web`, `world`, `gateway`, `tunnel`, and `zone` local process smoke path.
- Redis-backed web register/login through `libs/redis_cli`.
- Login options returned after auth.
- Gateway login and post-login game packet forwarding.

Not implemented yet:

- Public client protocol parser.
- Long-lived internal RPC client connections.
- Gateway-to-client push queue.
- Production password hashing and token issuance.
- Redis-backed role/account schema beyond the first account credential store.

## Next Implementation Step

Next hardening steps are password hashing/token issuance, a persistent role/account schema, and replacing one-shot `RpcNetworkClient` calls with long-lived internal clients where useful.
