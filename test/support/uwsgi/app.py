from time import sleep


def application(env, start_response):
    sleep(2)

    start_response("200 OK", [("Content-Type", "text/html")])

    return [b"Hello World"]
