// Session storage (this device): IndexedDB, because sessions carry audio
// takes that are far too big for localStorage. Two stores: "sessions"
// (JSON, keyed by name) and "audio" (Float32Array takes, keyed by id).
// Every call degrades to a rejected promise the UI reports, never a crash.

const DB = "adam-workshop";
let dbPromise = null;

function open() {
  if (!dbPromise) {
    dbPromise = new Promise((resolve, reject) => {
      if (!("indexedDB" in self)) return reject(new Error("this browser has no IndexedDB"));
      const req = indexedDB.open(DB, 1);
      req.onupgradeneeded = () => {
        req.result.createObjectStore("sessions");
        req.result.createObjectStore("audio");
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error || new Error("IndexedDB unavailable"));
    });
    dbPromise.catch(() => (dbPromise = null));
  }
  return dbPromise;
}

async function tx(store, mode, fn) {
  const db = await open();
  return new Promise((resolve, reject) => {
    const t = db.transaction(store, mode);
    const req = fn(t.objectStore(store));
    t.oncomplete = () => resolve(req?.result);
    t.onerror = () => reject(t.error);
    t.onabort = () => reject(t.error || new Error("storage aborted (full?)"));
  });
}

export const sessions = {
  list: () => tx("sessions", "readonly", (s) => s.getAllKeys()),
  get: (name) => tx("sessions", "readonly", (s) => s.get(name)),
  put: (name, session) => tx("sessions", "readwrite", (s) => s.put(session, name)),
  remove: (name) => tx("sessions", "readwrite", (s) => s.delete(name)),
};

export const audio = {
  get: (id) => tx("audio", "readonly", (s) => s.get(id)),
  put: (id, samples) => tx("audio", "readwrite", (s) => s.put(samples, id)),
};
