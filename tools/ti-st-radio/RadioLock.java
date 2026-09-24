import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;

public final class RadioLock {
    private static RadioLock currentClaim;
    private final RandomAccessFile file;
    private final FileLock lock;

    private RadioLock(RandomAccessFile file, FileLock lock) {
        this.file = file;
        this.lock = lock;
    }

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
