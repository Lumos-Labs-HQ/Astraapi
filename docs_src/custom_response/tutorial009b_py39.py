from astraapi import AstraAPI
from astraapi.responses import FileResponse

some_file_path = "large-video-file.mp4"
app = AstraAPI()


@app.get("/", response_class=FileResponse)
async def main():
    return some_file_path
