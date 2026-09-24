import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;

/** Holds a nonblocking, cross-process claim on a shared radio lock file. */
public final class RadioLock {
    private static RadioLock currentClaim;
    private final RandomAccessFile file;
    private final FileLock lock;

    /** Holds the file and lock for an acquired radio claim. */
    private RadioLock(RandomAccessFile file, FileLock lock) {
        this.file = file;
        this.lock = lock;
    }

    /**
     * Tries to claim the shared lock file without waiting for another radio.
     * Keep the returned claim until shutdown completes or startup fails.
     *
     * @param path the persistent lock file shared with native radio clients
     * @return the claim, or null if this process or another holds the lock
     * @throws IOException if the lock file cannot be opened or locked
     */
    public static synchronized RadioLock tryClaim(File path) throws IOException {
        if (currentClaim != null) {
            return null;
        }
        RandomAccessFile file = new RandomAccessFile(path, "rw");
        FileLock lock;
        try {
            lock = file.getChannel().tryLock();
        } catch (OverlappingFileLockException exception) {
            file.close();
            return null;
        } catch (IOException exception) {
            file.close();
            throw exception;
        }
        if (lock == null) {
            file.close();
            return null;
        }
        currentClaim = new RadioLock(file, lock);
        return currentClaim;
    }

    /**
     * Releases this claim and closes its file; does nothing if it is no longer current.
     *
     * @throws IOException if releasing the lock or closing the file fails
     */
    public void release() throws IOException {
        synchronized (RadioLock.class) {
            if (currentClaim != this) {
                return;
            }
            try {
                lock.release();
            } finally {
                try {
                    file.close();
                } finally {
                    currentClaim = null;
                }
            }
        }
    }
}
