"""HTML resource handling utilities."""

from pkg_resources import resource_filename, resource_string


def replace_references(text):
    """Replace {{ reference }} with the content of the referenced resource."""
    begin = 0
    while True:
        begin = text.find(b"{{", begin)
        if begin < 0:
            break
        end = text.find(b"}}", begin) + 2

        placeholder = text[begin:end]
        reference = placeholder[2:-2].strip()
        resource = resource_string("austin.html", reference.decode())

        text = text.replace(placeholder, replace_references(resource))

        begin += len(resource)

    return text


def replace_links(text):
    """Replace [[ link ]] with a link to the referenced resource."""
    begin = 0
    while True:
        begin = text.find(b"[[", begin)
        if begin < 0:
            break
        end = text.find(b"]]", begin) + 2

        placeholder = text[begin:end]
        reference = placeholder[2:-2].strip().decode()
        link = "file://" + resource_filename("austin.html", reference)

        text = text.replace(placeholder, link.encode())

        begin += len(link)

    return text


def load_site():
    template = resource_string("austin.html", "index.html")

    index = replace_links(replace_references(template))

    return index
