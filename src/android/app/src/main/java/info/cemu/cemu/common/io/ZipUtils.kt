package info.cemu.cemu.common.io

import java.io.FileOutputStream
import java.io.InputStream
import java.io.IOException
import java.nio.file.Path
import java.nio.file.Paths
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream

fun unzip(stream: InputStream, targetDir: String) {
    try {
        ZipInputStream(stream).use { zipInputStream ->
            val buffer = ByteArray(8192)

            var zipEntry: ZipEntry? = zipInputStream.nextEntry

            while (zipEntry != null) {
                try {
                    extractZipEntry(zipInputStream, zipEntry, buffer, targetDir)
                } catch (e: Exception) {
                    // Log error but continue with other entries
                    e.printStackTrace()
                }
                zipInputStream.closeEntry()
                zipEntry = zipInputStream.nextEntry
            }
        }
    } catch (e: Exception) {
        e.printStackTrace()
        throw e
    }
}

fun unzip(stream: InputStream, targetDir: Path) = unzip(stream, targetDir.toString())

private fun extractZipEntry(
    zipInputStream: ZipInputStream,
    zipEntry: ZipEntry,
    buffer: ByteArray,
    targetDir: String,
) {
    val targetDirPath = Paths.get(targetDir).toAbsolutePath().normalize()
    val entryPath = targetDirPath.resolve(zipEntry.name).normalize()
    
    // Zip Slip vulnerability protection
    if (!entryPath.startsWith(targetDirPath)) {
        throw IOException("Zip entry is outside of the target directory: ${zipEntry.name}")
    }

    val file = entryPath.toFile()
    if (zipEntry.isDirectory) {
        file.apply { if (!isDirectory) mkdirs() }
        return
    }
    
    // Ensure parent directories exist
    file.parentFile?.let {
        if (!it.exists()) {
            it.mkdirs()
        }
    }

    FileOutputStream(file).use { fileOutputStream ->
        var bytesRead: Int
        while ((zipInputStream.read(buffer).also { bytesRead = it }) > 0) {
            fileOutputStream.write(buffer, 0, bytesRead)
        }
    }
}
