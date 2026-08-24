# Security policy

## Supported versions

Mocara is beta software. Security fixes currently target the latest `0.2.x` source on the default branch.

## Report a vulnerability

Use the repository's **Security -> Report a vulnerability** flow to send a private GitHub security advisory. Do not open a public issue for an unpatched vulnerability or include credentials, tokens, private model files, or proprietary project content in a report.

Include the affected commit, environment, reproduction steps, expected impact, and the smallest safe proof of concept. We will acknowledge the report, validate it, and coordinate disclosure through the advisory.

## Intended trust boundary

- The FastAPI sidecar must bind only to `127.0.0.1`.
- `X-Mocara-Client` is an anti-browser request gate, not user authentication. Other local processes are inside the current trust boundary.
- The sidecar port must not be forwarded to a LAN, public interface, container bridge, or internet-facing proxy.
- Prompt text, JSON requests, BVH files, filesystem paths, and model outputs are untrusted at their boundaries and must remain bounded and validated.
- Sidecar shutdown validates both a PID and an ownership token before signalling a process.
- Hugging Face tokens and downloaded model files belong in the user's environment or cache, never in this repository or logs.
- WSL setup downloads executable Python dependencies and Kimodo source. Pins should move only through reviewed changes with license and supply-chain checks.

The service is designed for a single user on one workstation. Multi-user hosts, remote services, and packaged-game networking require a different authentication and isolation design.
