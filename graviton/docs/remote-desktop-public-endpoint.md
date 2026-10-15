# remote_broker: a shared public endpoint for :10902

Proposal for #611 (successor to #547). Companion to
[remote-desktop-broker.md](remote-desktop-broker.md) (the daemon) and
[remote-desktop-unified-design.md](remote-desktop-unified-design.md) (the
protocol roadmap). This document is the *design and the threat model*; the
networking itself is code, in
[`graviton/pipeline/lib/ops-stack.ts`](../pipeline/lib/ops-stack.ts).

**Nothing in AWS was changed to write this.** Every fact below came from a
`describe-*` / `list-*` / `get-*` call or from an SSM command that only read.
The endpoint is **off by default** and is applied by a human running the
commands in [Applying it](#applying-it).

## The problem, restated against measured reality

#611 says `:10902` is reachable only over the on-prem VPN. That is the right
conclusion but the issue body's specifics have drifted, so start from what is
actually true today (2026-10-15, account 668984504585, us-west-2).

**The security group allows `10.0.0.0/8`, not `10.0.0.0/16`.**

```
$ aws ec2 describe-security-groups --group-ids sg-0efc97cc673949063 \
    --query 'SecurityGroups[0].IpPermissions[?FromPort==`10900`]'
[
    {
        "FromPort": 10900,
        "IpProtocol": "tcp",
        "IpRanges": [
            {
                "CidrIp": "10.0.0.0/8",
                "Description": "VPC+peers (user-approved 2026-10-12)"
            }
        ],
        "ToPort": 10902
    }
]
```

So any host inside `BuildVpc` (`10.128.0.0/16` ⊂ `10.0.0.0/8`) can already
reach the broker, regardless of its own security group. Confirmed by connecting
from an unrelated in-VPC instance:

```
$ aws ssm send-command --instance-ids i-0b366c7e32e8a9c33 \
    --document-name AWS-RunShellScript \
    --parameters file:///tmp/probe611.json ...
TCP_10902_OPEN
CONNECTED(00000003)
New, TLSv1.3, Cipher is TLS_AES_256_GCM_SHA384
subject=CN=shredder
issuer=CN=shredder
verify error:num=18:self signed certificate
```

That is the gap precisely: **in-VPC is open, the internet is closed, and there
is nothing in between.** A reviewer on a laptop has no path at all.

**There is no load balancer in the account.** Nothing to extend:

```
$ aws elbv2 describe-load-balancers --query 'LoadBalancers[].LoadBalancerName'
[]
$ aws elb describe-load-balancers --query 'LoadBalancerDescriptions[].LoadBalancerName'
[]
```

(`describe-target-groups` does return five `k8s-istioing-*` groups, but each has
`LoadBalancerArns: []` and they sit in two unrelated VPCs —
`vpc-09ad79cf4d2cbf6ab` and `vpc-0b12d173a4262b120`. Leftovers from a k8s/istio
experiment, not substrate for this.)

**`:10900` is not exposed, even in-VPC.** The same probe to the app_server port
gets `Connection refused`: `RemoteHWInterface`'s RP listener is loopback-only,
so the broker is the sole network-facing path. The SG rule spanning
10900-10902 is wider than the daemon it fronts.

**The public subnets already exist.** This is the part nobody had noted, and it
is why this change is small:

```
$ aws ec2 describe-subnets --filters Name=vpc-id,Values=vpc-0249d1b95a3deb8d4 \
    --query 'Subnets[].[Tags[?Key==`Name`].Value|[0],SubnetId,AvailabilityZone,CidrBlock,MapPublicIpOnLaunch]' \
    --output text | sort
DebeosOpsStack/BuildVpc/buildersSubnet1  subnet-02f8f7929c0cd9607  us-west-2a  10.128.4.0/22   False
DebeosOpsStack/BuildVpc/buildersSubnet2  subnet-0c98257f993bfc1b2  us-west-2b  10.128.8.0/22   False
DebeosOpsStack/BuildVpc/buildersSubnet3  subnet-0d4f6b196024e9b92  us-west-2c  10.128.12.0/22  False
DebeosOpsStack/BuildVpc/publicSubnet1    subnet-0bed35f5ed7da0f76  us-west-2a  10.128.0.0/24   True
DebeosOpsStack/BuildVpc/publicSubnet2    subnet-0ba4ecc9d823a5b35  us-west-2b  10.128.1.0/24   True
DebeosOpsStack/BuildVpc/publicSubnet3    subnet-093ff4b80bc622eda  us-west-2c  10.128.2.0/24   True
```

Their route tables carry a default route to `igw-0c81e86a8fd5ffd97`. An
internet-facing load balancer needs no new VPC, subnet, IGW or route — only the
load balancer itself.

**Desktops have no public IP of their own**, so there is no "just assign an EIP"
shortcut already half-built: all five running instances tagged
`Project=haiku-graviton` report `PublicIpAddress: None` and sit in the private
builder subnets (`buildersSubnet1` and `buildersSubnet2` today, both
`MapPublicIpOnLaunch: false`). Which AZ a desktop lands in is not fixed, which
is why the endpoint needs cross-zone.

**Certificates and tokens are per-instance, generated at first boot.** This
matters for the threat model, so it was verified rather than assumed. Two
instances launched from the *same* AMI (`ami-0eee11d43faa67d8f`) serve different
certificates whose `notBefore` matches each instance's own launch time:

```
10.128.8.4    SHA256 Fingerprint=EF:27:...   notBefore=Oct 13 21:02:55 2026 GMT   (launched 21:02:43)
10.128.8.178  SHA256 Fingerprint=19:8E:...   notBefore=Oct 13 20:58:33 2026 GMT   (launched 20:58:05)
```

Nothing secret is baked into the AMI. Each desktop has its own keypair, its own
fingerprint and its own token.

**L7 termination has no materials today, independent of whether it is a good
idea.** There is no DNS zone and no usable certificate:

```
$ aws route53 list-hosted-zones --query 'HostedZones[].Name'
[]
$ aws acm list-certificates --query 'CertificateSummaryList[].[DomainName,Status,RenewalEligibility,InUse]'
[[ "*.debene.xyz", "EXPIRED", "INELIGIBLE", false ]]
```

## The approach: an NLB doing TCP passthrough on :10902

One internet-facing Network Load Balancer, one TCP listener on `:10902`, one
instance-target target group that **ships empty**, and a dedicated security
group whose ingress is an explicit CIDR allowlist. The whole block is gated on
deploy-time context, so an ordinary `cdk deploy` of `DebeosOpsStack` creates
none of it.

```
reviewer's laptop                      BuildVpc 10.128.0.0/16
+-----------------+                +-----------------------------------+
| rdcapture.py    |   wss :10902   |  publicSubnet1 (us-west-2a)       |
|   --pin <fp>    +--------------->|   NLB (TCP passthrough)           |
| or the browser  |   TLS end to   |     |  cross-zone on              |
+-----------------+   end, intact  |     v                             |
                                   |  buildersSubnet2 (us-west-2b)     |
                                   |   remote_broker :10902 (TLS+WS)   |
                                   |     |  loopback only              |
                                   |     v                             |
                                   |   app_server :10900               |
                                   +-----------------------------------+
```

The TLS session the client negotiates terminates **on the desktop**, inside the
broker. The load balancer forwards bytes and never holds a key.

### Why passthrough and not an ALB

The broker already terminates TLS and its trust model is **certificate pinning**
(`broker.fingerprint`), not a CA chain — see
[remote-desktop-broker.md](remote-desktop-broker.md) §"Certificate and
pinning". Three consequences, each of which rules out L7 termination on its
own:

1. **Pinning would start verifying the wrong thing.** `rdcapture.py --pin <fp>`
   exists to assert "I am talking to *that* desktop". With an ALB in the path
   the client pins, at best, the load balancer — which fronts every desktop. The
   one check that distinguishes your desktop from someone else's becomes a check
   that you reached AWS. The feature does not degrade; it inverts.

2. **The credential and the session cookie would exist in cleartext inside the
   proxy.** `RP_AUTHENTICATE` carries the shared token, and the broker then
   presents app_server's per-boot `RP_SESSION_COOKIE` on the loopback hop. An
   ALB decrypts, inspects and re-encrypts, so both pass through it in the clear
   and into its access logs' blast radius. Passthrough keeps them inside a
   session only the two endpoints can read.

3. **It would collapse the broker's per-source rate limiter.** The limiter keys
   on the raw client address — `RemoteBroker.cpp:1433`,
   `context->peerAddress = peer.sin_addr.s_addr;` — with a 64-entry table and
   `1 << min(failures, 6)` second backoff. Behind an L7 proxy every client
   arrives as the proxy's address, so all of them share one bucket: one attacker
   locks out every legitimate user, and the attacker's own backoff is diluted by
   everyone else's success. An NLB with `targetType: instance` and
   `preserve_client_ip.enabled = true` delivers the real client address, and the
   limiter keeps working as designed.

And the decisive, forward-looking reason:

4. **An ALB can never carry UDP, so it is a dead end.**
   [remote-desktop-unified-design.md](remote-desktop-unified-design.md) §4.2
   lists QUIC and WebRTC as transports and §10 schedules
   "QUIC/WebRTC transport" at **M5**. ALB is HTTP-only, forever. An NLB takes
   `UDP` and `TCP_UDP` listeners, so M5 adds a listener to *this* load balancer
   and keeps the same DNS name. Choosing an ALB now means throwing it away and
   re-doing the endpoint, with a name change, at M5.

### Why not the other candidates

- **CloudFront** is L7 termination with the same three problems plus a CDN's
  WebSocket idle limits, and it is useless for M5 UDP. It also needs the DNS
  zone and certificate that do not exist.
- **An Elastic IP per desktop** is the cheapest option ($0 while attached) and
  the worst fit for the issue: #611 asks for a *shared* endpoint. Per-instance
  addresses mean a new address and a new client config for every ephemeral
  desktop, which is the coordination cost the issue is complaining about.
  Nothing here forecloses it — it stays the right answer for a one-off demo.
- **Widening the existing SG to a public CIDR** cannot work: the desktops have
  no public IP and sit in private subnets. There is no address to widen *to*.
- **A bastion / SSH tunnel** works today and is the current workaround. It
  requires every viewer to hold credentials to the build account, which is a
  larger grant than "watch a desktop", and it cannot serve the browser client.

### Why cross-zone is on, and why only one public subnet

The NLB sits in **one** public subnet. AWS bills every in-use public IPv4
address, and a node per AZ buys only AZ redundancy for a test endpoint fronting
a single ephemeral desktop. Subnets can be added to a live NLB without
replacing it, so this is reversible.

Given one subnet, `crossZoneEnabled` is load-bearing rather than a default:
`haiku-launch` puts the desktop in whichever builder subnet it picks, usually a
different AZ from the NLB node. With cross-zone off, the endpoint would reach
the target only when the two happened to coincide.

### Why the TCP health check is safe

A TCP health check opens a connection and closes it without completing TLS. The
broker deliberately does **not** count that against the rate limiter
(`RemoteBroker.cpp:1098-1105`, "Deliberately NOT counted as an authentication
failure"), so health probes cannot lock out the load balancer's own subnet or
anyone else. A *TLS* health check would have been the wrong choice for a
different reason — the certificate is self-signed and per-instance, so no
validating prober can be configured to trust it.

## Threat model

State plainly what changes: **`:10902` on one chosen desktop becomes reachable
from the internet, from the allowlisted prefixes only, and the broker's shared
token becomes the only thing between a stranger and a live desktop session with
full keyboard and mouse control.**

### What authenticates it

The first payload on the WebSocket must be an RP-framed `RP_AUTHENTICATE` (=10)
carrying a shared token. The broker SHA-256s it and compares against the stored
digest with `CRYPTO_memcmp` (`RemoteBroker.cpp:968-972`), so the compare is
constant-time and the token is not stored in the clear. Pre-auth abuse is
bounded by:

| Control | Value | Site |
| --- | --- | --- |
| Handshake timeout | 15 s | `RemoteBroker.cpp:105` |
| Delay after a failed auth | 2 s | `RemoteBroker.cpp:109` |
| Concurrent handshakes (global) | 8 | `RemoteBroker.cpp:111` |
| Per-source backoff | `1 << min(failures, 6)` s, 15-min decay | `RateLimiter::_Backoff` |
| Rate-limiter table | 64 entries, oldest evicted | `Entry fEntries[64]` |

That is a competent set of controls and it is **not sufficient on its own** for
an unrestricted public listener. Two specific reasons:

**The credential has no expiry, no scope and no central revocation.** One
long-lived string grants full control of a desktop. Rotating it means editing
`/boot/system/settings/remote_desktop/token` on the instance and telling every
viewer. There is no audit trail tying a session to a person, because every
viewer presents the identical secret. Guessing is not the threat — leaking is: a
token pasted into a chat, a shell history, or a CI log is game over for that
desktop until someone notices.

**The 8-slot global handshake gate becomes a denial-of-service lever once the
listener is public.** Eight concurrent handshakes with a 15-second timeout means
roughly 0.5 connections per second of *silent* TCP connections — no TLS, no
auth, no rate-limiter entry, because a stalled handshake is not an auth failure
— keeps every slot full and locks out legitimate clients. This is tracked
separately as a prerequisite; see [Follow-ups](#follow-ups).

### What the design does about it

The mitigation is to not rely on the token alone:

- **The CIDR allowlist is the primary control.** The endpoint's security group
  permits `:10902` from explicitly listed prefixes. Reaching the token requires
  first being on the allowlist.
- **"The internet" cannot be typed by accident.** `parseBrokerEndpointAllowList`
  (`ops-stack.ts:67`) rejects any prefix shorter than `/16` unless a second,
  separate `-c brokerEndpointAllowWorld=true` is also passed. `0.0.0.0/0` is
  reachable, but only as a deliberate, reviewable, two-flag decision.
- **A malformed allowlist fails the synth.** A typo does not silently open the
  endpoint, and does not silently close it either.
- **The target group ships empty.** Deploying the stack exposes nothing. A human
  registers exactly one instance, by id, and deregisters it when done.
- **`BuilderSg` is widened only to the endpoint's security group**, never to a
  CIDR. The blast radius of the new rule is the load balancer, not a network
  range. When the endpoint is torn down the rule's source ceases to exist.
- **Only `:10902`.** The endpoint does not carry the 10900-10902 span the
  hand-edited SG rule has; `:10900` stays loopback-only.

### What remains true after all that

- Anyone on an allowlisted prefix who holds the token gets the desktop. If the
  prefix is a corporate NAT, that is everyone behind it.
- The browser client cannot pin (`HaikuRemoteDesktop.html` has no mechanism for
  it), so browser users click through a self-signed-certificate warning. On a
  public endpoint that is the exact habit an interception attack needs. Prefer
  `rdcapture.py --pin` for anything sensitive; treat the browser path as
  convenience on a trusted network.
- A leaked token is not detectable from the broker's side. There is no session
  log that would distinguish a stranger from you.
- Therefore: **put nothing on an allowlisted-to-the-world endpoint that you
  would mind a stranger seeing.** These are ephemeral test desktops; keep them
  that way.

## Cost

A persistent public endpoint is a standing charge in an account whose test
instances are deliberately ephemeral. Rates from the pricing API
(`ServiceCode=AWSELB`, `location=US West (Oregon)`):

| Item | Rate | Monthly at 730 h |
| --- | --- | --- |
| `USW2-LoadBalancerUsage` | $0.0225 / NLB-hour | **$16.43** |
| `USW2-LCUUsage` | $0.006 / NLCU-hour | cents at interactive draw-op rates |
| Cross-AZ data processing | $0.01 / GB each direction | cents |
| In-use public IPv4 address | $0.005 / hour published | ~$3.65 per address |

The NLB-hour and NLCU rates are API-verified. The public-IPv4 figure is **not**:
`aws pricing get-products` returned an empty `PriceList` for both the
`IP Address` and `Public IPv4 Address` product families, so that line is the
published rate rather than a measured one. With one public subnet there is one
such address, so expect **roughly $16-20/month** while the endpoint exists, and
check the first bill rather than trusting this table.

Set against that: the stack already pays for a NAT gateway
(`nat-0cd738b47ba8d5149`, in `publicSubnet1`, roughly $33/month at the published
hourly rate plus data processing) that nothing in this change touches.

The design's answer to the cost is that **the endpoint does not exist unless
someone asks for it**. No context key, no resources, no bill — `cdk deploy` with
no extra flags produces a template with no load balancer in it, which is the
first of the seven tests. Teardown is a deploy without the flag.

## Applying it

**None of these commands have been run.** They mutate a production account and
are for a human to execute after reviewing the above. Run from
`graviton/pipeline`.

Review the diff before anything else:

```bash
cdk diff DebeosOpsStack -c brokerEndpointAllowCidrs=<your.public.ip>/32
```

Create the endpoint. Scope the allowlist to what you actually connect from; a
`/32` of your current public address is the right default. Multiple prefixes are
comma-separated.

```bash
cdk deploy DebeosOpsStack -c brokerEndpointAllowCidrs=<your.public.ip>/32
```

The deploy prints three outputs: `BrokerEndpointDnsName`,
`BrokerEndpointTargetGroupArn` and `BrokerEndpointSecurityGroupId`. Nothing is
reachable yet — the target group is empty.

Attach exactly one desktop:

```bash
aws elbv2 register-targets \
  --target-group-arn <BrokerEndpointTargetGroupArn> \
  --targets Id=<instance-id>

aws elbv2 describe-target-health \
  --target-group-arn <BrokerEndpointTargetGroupArn> \
  --query 'TargetHealthDescriptions[].[Target.Id,TargetHealth.State]'
```

Wait for `healthy`. Then fetch that instance's fingerprint and token over SSM
(they are per-instance, so they must come from the instance you registered) and
connect with the pin:

```bash
python3 graviton/scripts/rdcapture.py \
  --host <BrokerEndpointDnsName> --port 10902 --wss \
  --token-file <token> --pin <fingerprint>
```

The pin must still be the **desktop's** fingerprint, not the endpoint's — that
is the point of passthrough, and a pin failure here means something is
terminating TLS that should not be.

When finished, detach the desktop and then remove the endpoint:

```bash
aws elbv2 deregister-targets \
  --target-group-arn <BrokerEndpointTargetGroupArn> \
  --targets Id=<instance-id>

cdk deploy DebeosOpsStack
```

The final deploy, with no context flag, deletes the load balancer, listener,
target group and security group, and removes the `BuilderSg` rule that
referenced it. The monthly charge stops.

To rotate the allowlist without touching the target, re-deploy with a different
`brokerEndpointAllowCidrs`.

## Where the code lives

All of it is in [`graviton/pipeline/lib/ops-stack.ts`](../pipeline/lib/ops-stack.ts),
the CDK stack that already owns `BuildVpc`, `BuilderSg` and the VPN:

| What | Line |
| --- | --- |
| `BROKER_PORT` | 45 |
| `parseBrokerEndpointAllowList` (pure, unit-tested) | 67 |
| Context gate | 388 |
| `BrokerEndpointSg` + the `BuilderSg` widening | 392 |
| `BrokerNlb` | 410 |
| `BrokerTargets` | 430 |
| `BrokerListener` | 451 |
| Outputs | 460 |

Deploy-time context keys:

| Key | Meaning |
| --- | --- |
| `brokerEndpointAllowCidrs` | Comma-separated IPv4 prefixes. Absent or empty = **no endpoint at all**. |
| `brokerEndpointAllowWorld` | `true` to permit a prefix shorter than `/16`. Required in addition to the above; never set it casually. |

Seven tests in [`graviton/pipeline/test/pipeline.test.ts`](../pipeline/test/pipeline.test.ts)
hold the invariants: nothing internet-facing by default; internet-facing in a
public subnet when asked; the listener is TCP with no certificate; targets are
instance-type with client IP preserved and none registered at deploy;
`BuilderSg` is widened only to the endpoint SG; a short prefix needs the second
flag; a malformed allowlist fails the synth. Each was mutation-checked — broken
deliberately, observed to fail, reverted.

## Follow-ups

Daemon-side work this endpoint argues for, which is **not** in this change
(`src/servers/remote_broker/` is being modified concurrently for #551 and #549):

1. **The 8-slot handshake gate needs to be per-source, not global** before the
   listener is opened to anything broad. `kMaxConcurrentHandshakes = 8`
   (`RemoteBroker.cpp:111`, enforced at `:1400-1403`) is a global counter, so
   ~0.5 silent connections per second denies service to everyone. Filed
   separately as a prerequisite and referenced from #611.
2. **Token rotation and expiry.** A single long-lived credential with no
   revocation is the weakest part of the public story. Even a coarse "tokens
   expire N hours after the broker starts" would bound a leak.
3. **A session log.** One line per authenticated session with the preserved
   client address — which passthrough makes available — would turn "a token
   leaked" from undetectable into noticeable.
4. **Pinning for the browser client.** Until then the browser path is
   accept-once and should not be used across the public internet for anything
   that matters.

Observations noted while measuring, not defects being claimed here:

- The live `sg-0efc97cc673949063` has **drifted from this CDK stack**. The
  three remote-desktop rules were hand-edited: the stack writes `onPremCidr`
  (default `10.0.0.0/16`) with descriptions like "remote desktop (app_server +
  broker) from on-prem over VPN", while the live rules say `10.0.0.0/8` /
  "VPC+peers (user-approved 2026-10-12)", and the live ICMP rule has no
  description at all. The next `cdk deploy DebeosOpsStack` will try to
  reconcile this. Worth looking at before deploying anything to this stack.
- `remote-desktop-broker.md` states that TLS and WebSocket handshake failures
  count toward the per-source backoff. `RemoteBroker.cpp:1098-1105` says the
  opposite, in a comment, deliberately. The code is right and the doc is stale;
  not edited here to avoid colliding with the concurrent broker work.
- The certificate's subject is `CN=shredder` on every instance — the hostname is
  not being set from DHCP, so all desktops claim the same name. Harmless under
  pinning, confusing in logs.
- The certificate SAN is `DNS:shredder, IP Address:127.0.0.1`, with no interface
  address. #602's SAN fix is on `graviton` but the canonical AMI was baked
  2026-10-13, before it; it arrives with the next bake. Pinning does not care,
  but any future hostname-validating client will.
