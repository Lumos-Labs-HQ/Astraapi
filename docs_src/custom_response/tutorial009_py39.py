from astraapi import AstraAPI
from astraapi.responses import FileResponse

some_file_path = "large-video-file.mp4"
app = AstraAPI()


@app.get("/")
async def main():
    return FileResponse(some_file_path)
