import { observe } from "./misc-cases.js";
await Bun.write(process.argv[2] + "/misc.cases.json", JSON.stringify(await observe(Bun)));
console.log("misc observations written");
