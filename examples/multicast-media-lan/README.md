# Multicast Media LAN Example

## Files

- `sender.js` — sends timestamped binary chunks to a multicast group.
- `receiver.js` — joins multicast group and logs packet timing/size.

## Run

Terminal 1:

```bash
node receiver.js
```

Terminal 2:

```bash
node sender.js
```
