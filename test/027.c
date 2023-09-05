int binarySearch(int arr[], int l, int r, int x) {
    if (r >= l) {
        int mid = l + (r - l) / 2;
        if (arr[mid] == x)
            return mid;

        if (arr[mid] > x)
            return binarySearch(arr, l, mid - 1, x);

        return binarySearch(arr, mid + 1, r, x);
    }

    // We reach here when element is not present in array.
    return -1;
}

int arr[] = { 2, 3, 4, 10, 40, 50, 60, 70, 80, 90 };
int main() {
    int idx = 7;
    int search = binarySearch(arr, 0, 9, arr[idx]);
    return search == idx;
}