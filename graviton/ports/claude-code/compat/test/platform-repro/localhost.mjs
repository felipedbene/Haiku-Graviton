// What the system resolver answers for "localhost" (getaddrinfo order).
import dns from "node:dns/promises";
console.log(JSON.stringify({ platform: process.platform, localhost: await dns.lookup("localhost", { all: true }) }));
